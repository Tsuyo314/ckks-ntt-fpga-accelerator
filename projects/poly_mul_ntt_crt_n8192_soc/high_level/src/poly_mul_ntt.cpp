#include "ap_int.h"

// CRT/RNS分解対応 (Lチャネル) + Shoup乗算バタフライ版 NTT多項式乗算アクセラレータ。
//
// ベースは poly_mul_ntt_openfhe_n8192_soc (単一モジュラスQ, 乗算はすべてBarrett還元)
// と同じ OpenFHE方式 (CT順変換/GS逆変換, 負巡回) だが、以下の2点を拡張する:
//
//   1. バタフライ内の回転因子乗算・逆変換末尾のN^-1スケーリングを Barrett還元 -> Shoup乗算
//      に変更した。回転因子・N^-1 は事前計算済みの固定定数なので、実行時変数との積は
//      「固定定数側だけ事前計算した高位ワードで商を見積もる」Shoupの手法が使え、
//      条件付き減算もBarrettの最大2回に対しShoupは最大1回で済む。
//      点ごとの乗算 (A[i]*B[i]) は両方とも実行時変数なのでShoupの前提
//      (乗数の片方が既知の定数) が成立せず、Barrett還元のまま。
//
//   2. モジュラスを単一の大きい素数Qではなく、L個のNTT向き素数 q0..q(L-1) の積として扱う
//      (CRT/RNS分解)。Lチャネル分の残差多項式への分解 (A mod qi 等)、および最終的な
//      CRT復元 (中国剰余定理による統合) はPythonホスト側が担当する
//      (fpga/lib/ntt_reference.py の crt_reconstruct/negacyclic_poly_mul_rns 参照)。
//
// [設計変更] 当初は Q_TAB[NTT_L]/RFWD[NTT_L][NTT_N] 等をチャネル数ぶん合成時にROMへ
// 焼き込み、ch レジスタで行選択する構成だったが、これは「論理的な時分割」であって
// 物理的にはLチャネル分のBRAMを同時に確保してしまい、HLSコア単体でBRAM使用量が
// 332/270ブロック(122%)となりxc7a100tに収まらなかった
// (詳細は TROUBLESHOOTING.md 5節参照)。
//
// そこで本バージョンでは、モジュラス・回転因子テーブルを合成時ROMにせず、
// 呼び出しごとに mem 経由で実行時ロードする「2フェーズ構成」に変更した:
//   - nfwd (順変換) は RFWD/RFWD_SH しか使わない、ninv (逆変換) は RINV/RINV_SH しか
//     使わない、という自然な境界を利用し、オンチップの回転因子テーブル用配列は
//     TAB/TABSH の2本だけを使い回す (4本を同時に持たない)。
//   - phase=0 (順変換+点ごとの乗算): mem から A, B, TAB(=RFWD), TABSH(=RFWD_SH) を
//     ロードし、nfwd(A), nfwd(B), 点ごとの乗算(Barrett) までを行う。結果はAに
//     オンチップ(static/BRAM)のまま保持し、mem には書き戻さない。
//   - phase=1 (逆変換): ホスト側が mem の同じテーブル領域を RINV/RINV_SH で
//     上書き済みという前提で、TAB/TABSHを再ロードしてから ninv(A) を実行し、
//     結果を mem に書き戻す。
// これにより、共有メモリ・オンチップBRAMいずれも「テーブル領域は最大1チャネル分」
// で済み、Lチャネル分を同時に持つことがなくなる。
// A/B/TAB/TABSH を static にしているのは、Start() をまたいで (phase=0 -> phase=1)
// BRAM内容を保持させるため (AP_CTRLのFSMリセットはBRAM内容自体はクリアしない)。
//
// q/mu/ninv/ninvsh (チャネルごとのモジュラス・Barrett定数・N^-1・そのShoup定数) も
// 合成時ROM (Q_TAB等) ではなく、呼び出しごとにホストが送るスカラー引数にした。
// これによりこのIPは「どのチャネルの計算をしているか」を一切知らない、完全に
// チャネル非依存の汎用単一モジュラスNTT積コアになっている
// (fpga/lib/generated/ntt_params_n8192_crt2_q60.json の channels[] から、
// ホスト側 (host/host_crt.py) が該当チャネルの値を読んで送る)。
//
// 配列名・関数名・ループラベルは短く保つ: HLSは関数名+ループラベル+配列名を
// 連結した名前でROM/RAM初期化用.datファイルを生成するため、長い名前はWindowsの
// MAX_PATH(260文字)制限に抵触しやすい (poly_mul_ntt_openfhe_n8192_soc/TROUBLESHOOTING.md参照)。

typedef ap_uint<64>  coef_t;   // 60ビットのQを収める64ビットコンテナ (全チャネル共通)
typedef ap_uint<128> dcoef_t;  // u*v (< Q^2, <=120ビット) を保持する2倍幅コンテナ

#define NTT_N      8192
#define NTT_LOG2N  13
#define NTT_W_BITS 60   // 全チャネル共通 (poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md 3節参照)

static inline coef_t add_mod(coef_t u, coef_t v, coef_t q) {
#pragma HLS INLINE
    ap_uint<65> t = (ap_uint<65>)u + (ap_uint<65>)v;
    if (t >= (ap_uint<65>)q) t -= (ap_uint<65>)q;
    return (coef_t)t;
}

static inline coef_t sub_mod(coef_t u, coef_t v, coef_t q) {
#pragma HLS INLINE
    ap_uint<65> t = (ap_uint<65>)u + (ap_uint<65>)q - (ap_uint<65>)v;
    if (t >= (ap_uint<65>)q) t -= (ap_uint<65>)q;
    return (coef_t)t;
}

// Barrett還元: x (< q^2, <=120ビット) を mod q に還元する。点ごとの乗算 (両オペランドとも
// 実行時変数) にのみ使う。mu = floor(2^(2*NTT_W_BITS)/q) はホストが事前計算して送る値。
// qhatは真の商より最大1小さい場合があるため、減算後に最大2回の補正が必要。
static inline coef_t barrett_reduce(dcoef_t x, coef_t q, coef_t mu) {
#pragma HLS INLINE
    ap_uint<192> t    = x * (coef_t)mu;                  // <=120 + 64 = 184ビット
    coef_t       qhat = (coef_t)(t >> (2 * NTT_W_BITS));
    dcoef_t      r    = x - (dcoef_t)(qhat * q);
    if (r >= (dcoef_t)q) r -= (dcoef_t)q;
    if (r >= (dcoef_t)q) r -= (dcoef_t)q;
    return (coef_t)r;
}

static inline coef_t mul_mod_barrett(coef_t u, coef_t v, coef_t q, coef_t mu) {
    return barrett_reduce((dcoef_t)u * (dcoef_t)v, q, mu);
}

// Shoup乗算: 固定の乗数 w (回転因子やN^-1) について事前計算した wsh = floor(w*2^64/q) を
// 使い、実行時変数 u との積 u*w mod q を求める。Barrettと違いqhatの誤差補正は最大1回で済み、
// また u*w・qhat*q はいずれも下位64ビットの折り返し演算だけで求まる (ap_uint<64>への代入で
// 自動的に切り捨てられる)。前提: q < 2^63 (60ビット素数なので満たす)。
static inline coef_t mul_mod_shoup(coef_t u, coef_t w, coef_t wsh, coef_t q) {
#pragma HLS INLINE
    ap_uint<128> hi   = (ap_uint<128>)u * (ap_uint<128>)wsh;
    coef_t       qhat = (coef_t)(hi >> 64);         // mulhi(u, wsh)
    coef_t       t    = (coef_t)(u * w);            // mullo(u, w)   (64ビットに折り返し)
    coef_t       qq   = (coef_t)(qhat * q);          // mullo(qhat,q) (64ビットに折り返し)
    coef_t       r    = t - qq;                       // 64ビット折り返し減算
    if (r >= q) r -= q;
    return r;
}

// 順変換 (Cooley-Tukey, 自然順 -> ビット反転順)。tab/tabsh は呼び出し側 (トップ関数) が
// mem からロード済みの回転因子/Shoup係数テーブル (このコアは自分がどのチャネルの
// 計算をしているか一切知らない)。
static void ntt_fwd(coef_t a[NTT_N], coef_t tab[NTT_N], coef_t tabsh[NTT_N], coef_t q) {
#pragma HLS INLINE off
    const int half = NTT_N >> 1;

SF:
    for (int stage = 0; stage < NTT_LOG2N; stage++) {
#pragma HLS LOOP_TRIPCOUNT min=13 max=13
        int m    = 1 << stage;
        int tlog = NTT_LOG2N - 1 - stage;
        int t    = 1 << tlog;
        int mask = t - 1;

    BF:
        for (int k = 0; k < half; k++) {
            // IIはHLSに選ばせる (poly_mul_ntt_openfhe_n8192_soc と同じ理由: UART転送
            // 往復が支配的ボトルネックなので、パイプライン段数が多少増えても影響は軽微)。
#pragma HLS PIPELINE
            int i       = k >> tlog;
            int j_local = k & mask;
            int j       = (i << (tlog + 1)) + j_local;

            coef_t w   = tab[m + i];
            coef_t wsh = tabsh[m + i];
            coef_t hi  = mul_mod_shoup(a[j + t], w, wsh, q);
            coef_t lo  = a[j];
            a[j]     = add_mod(lo, hi, q);
            a[j + t] = sub_mod(lo, hi, q);
        }
    }
}

// 逆変換 (Gentleman-Sande, ビット反転順 -> 自然順)。N^-1によるスケーリングを含む。
// tab/tabsh はこの時点でホストがRINV/RINV_SHで上書き済みの領域からロードされたもの。
static void ntt_inv(coef_t a[NTT_N], coef_t tab[NTT_N], coef_t tabsh[NTT_N],
                     coef_t q, coef_t ninv, coef_t ninvsh) {
#pragma HLS INLINE off
    const int half = NTT_N >> 1;

SI:
    for (int s = 0; s < NTT_LOG2N; s++) {
#pragma HLS LOOP_TRIPCOUNT min=13 max=13
        int mask = (1 << s) - 1;
        int h    = NTT_N >> (s + 1);
        int t    = 1 << s;

    BI:
        for (int k = 0; k < half; k++) {
#pragma HLS PIPELINE
            int i       = k >> s;
            int j_local = k & mask;
            int j       = (i << (s + 1)) + j_local;

            coef_t w   = tab[h + i];
            coef_t wsh = tabsh[h + i];
            coef_t lo  = a[j];
            coef_t hi  = a[j + t];
            a[j]     = add_mod(lo, hi, q);
            a[j + t] = mul_mod_shoup(sub_mod(lo, hi, q), w, wsh, q);
        }
    }

SC:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        a[i] = mul_mod_shoup(a[i], ninv, ninvsh, q);
    }
}

// トップ関数: mem の内容はレイアウト mem[0,N)=A, mem[N,2N)=B, mem[2N,3N)=TAB,
// mem[3N,4N)=TABSH。phase により以下の2通りに分岐する
// (MicroBlaze (software/src/main.c) がリクエストごとに q/mu/ninv/ninvsh/phase を
// 設定してから Start() する):
//
//   phase=0: A,B,TAB(=RFWD),TABSH(=RFWD_SH) をロードし、ntt_fwd(A), ntt_fwd(B),
//            点ごとの乗算(Barrett) を行う。結果はAにオンチップのまま保持し、
//            mem への書き戻しはしない。
//   phase=1: TAB(=RINV),TABSH(=RINV_SH) を再ロードし (mem[2N,4N)をホストが
//            上書き済み)、ntt_inv(A) を実行して mem[0,N) に結果を書き戻す。
void poly_mul_ntt_crt(coef_t *mem, coef_t q, coef_t mu, coef_t ninv, coef_t ninvsh, ap_uint<1> phase) {
#pragma HLS INTERFACE s_axilite port=return  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mem     bundle=CTRL
#pragma HLS INTERFACE s_axilite port=q       bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mu      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ninv    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ninvsh  bundle=CTRL
#pragma HLS INTERFACE s_axilite port=phase   bundle=CTRL
// バースト長を256から64に縮小: gmemのAXIバーストFIFOが占めるBRAMを削減するため
// (Block Design全体でBRAMが289/270ブロックとわずかに超過したことが判明したための対策。
// UART転送(115200bps)が支配的ボトルネックのため、バースト長を縮小しても全体性能への
// 影響は無視できる。TROUBLESHOOTING.md参照)。
#pragma HLS INTERFACE m_axi     port=mem     bundle=gmem depth=32768 \
        max_read_burst_length=16 max_write_burst_length=16

    static coef_t A[NTT_N];
    static coef_t B[NTT_N];
    static coef_t TAB[NTT_N];
    static coef_t TABSH[NTT_N];
#pragma HLS BIND_STORAGE variable=A     type=RAM_2P impl=BRAM
#pragma HLS BIND_STORAGE variable=B     type=RAM_2P impl=BRAM
// TAB/TABSHは「RT/RTSループで書き込み -> ntt_fwd/ntt_invで読み出し」が排他的
// (同一サイクルでの同時読み書きが発生しない) なので RAM_1P で足りる。RAM_2Pに比べ
// 使用BRAM量が概ね半分になる (実測: Block Design全体でBRAM 289/270=107%だったのが
// この変更でチップ全体の予算内に収まる。TROUBLESHOOTING.md参照)。
#pragma HLS BIND_STORAGE variable=TAB   type=RAM_1P impl=BRAM
#pragma HLS BIND_STORAGE variable=TABSH type=RAM_1P impl=BRAM

RT:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        TAB[i] = mem[2 * NTT_N + i];
    }
RTS:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        TABSH[i] = mem[3 * NTT_N + i];
    }

    if (phase == 0) {
    RA:
        for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
            A[i] = mem[i];
        }
    RB:
        for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
            B[i] = mem[NTT_N + i];
        }

        ntt_fwd(A, TAB, TABSH, q);
        ntt_fwd(B, TAB, TABSH, q);

    PW:
        for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
            A[i] = mul_mod_barrett(A[i], B[i], q, mu);
        }
        // 結果はAにオンチップのまま保持。mem書き戻しはphase=1で行う。
    } else {
        ntt_inv(A, TAB, TABSH, q, ninv, ninvsh);

    WR:
        for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
            mem[i] = A[i];
        }
    }
}
