#include "ap_int.h"

// OpenFHE 方式 (CT-forward / GS-inverse, 負巡回) NTT による多項式乗算アクセラレータ。
// N=8192, Q は fpga/lib/ntt_params.py が生成した ntt_params_n8192.h 内の
// NTT_Q (60ビットNTT向き素数, Q = k*2N+1)。回転因子テーブル (RFWD/RINV) は
// projects/poly_mul_ntt_openfhe_soc/NTT_ALGORITHM.md と同じ規約
// (Table[bitrev(i)] = psi^i, 線形カウンタ m+i でインデックス) で生成済み。
//
// N=4 のトイ版 (poly_mul_ntt_openfhe_soc/high_level/src/poly_mul_ntt.cpp) との違い:
//   - coef_t は9ビットではなく64ビットコンテナ (Qは60ビット)
//   - mod演算に `%` を使うと汎用除算器が合成されてしまうため、
//     add_mod/sub_mod は条件付き1回減算、mul_mod は Barrett還元を使う
//   - バタフライは13段を手展開せず、シフト/マスクだけで添字計算するループにした
//     (割り算・剰余を使うと除算器が合成されるため、tは常に2のべき乗であることを
//     利用してシフトに置き換えている)
//   - 回転因子テーブルは ARRAY_PARTITION complete で配線化せず、BIND_STORAGE で
//     明示的にBRAM化する (N=4版は逆に「ROMを作らせない」ことが目的だった)
//   - トップ関数はAXI-Liteスカラー引数8個ではなく、共有AXI BRAMバッファを指す
//     m_axi ポインタ1本を取る (8192係数をレジスタ単位ではやり取りできないため)
//
// 関数名・ループラベル・配列名 (nfwd/ninv, SF/BF/SI/BI等, RFWD/RINV) は
// あえて短く付けている: HLSは「関数名+ループラベル+配列名」を連結した名前で
// ROM初期化用の.datファイルを生成するため、長い名前を使うとWindowsの
// MAX_PATH(260文字)制限に抵触する (実際に NTT_ROOT_FWD/INV +
// STAGE_INV_BUTTERFLY_INV という名前で発生し、278文字になったことを確認済み。
// TROUBLESHOOTING.md参照)。

typedef ap_uint<64>  coef_t;   // 60ビットのQを収める64ビットコンテナ
typedef ap_uint<128> dcoef_t;  // u*v (< Q^2, <=120ビット) を保持する2倍幅コンテナ

#include "ntt_params_n8192.h"  // coef_t 定義後にinclude (RFWD/RINVがcoef_t型のため)

static inline coef_t add_mod(coef_t u, coef_t v) {
#pragma HLS INLINE
    ap_uint<65> t = (ap_uint<65>)u + (ap_uint<65>)v;
    if (t >= (ap_uint<65>)NTT_Q) t -= (ap_uint<65>)NTT_Q;
    return (coef_t)t;
}

static inline coef_t sub_mod(coef_t u, coef_t v) {
#pragma HLS INLINE
    ap_uint<65> t = (ap_uint<65>)u + (ap_uint<65>)NTT_Q - (ap_uint<65>)v;
    if (t >= (ap_uint<65>)NTT_Q) t -= (ap_uint<65>)NTT_Q;
    return (coef_t)t;
}

// Barrett還元: x (< Q^2, <=120ビット) を mod Q に還元する。
// mu = floor(2^(2W)/Q) は ntt_params.py が事前計算した定数 (NTT_MU)。
// qhat は真の商より最大1小さい場合があるため、減算後に最大2回の補正が必要。
static inline coef_t barrett_reduce(dcoef_t x) {
#pragma HLS INLINE
    ap_uint<192> t    = x * (coef_t)NTT_MU;             // <=120 + 64 = 184ビット
    coef_t       qhat = (coef_t)(t >> (2 * NTT_W_BITS));
    dcoef_t      r    = x - (dcoef_t)(qhat * (coef_t)NTT_Q);
    if (r >= (dcoef_t)NTT_Q) r -= (dcoef_t)NTT_Q;
    if (r >= (dcoef_t)NTT_Q) r -= (dcoef_t)NTT_Q;
    return (coef_t)r;
}

static inline coef_t mul_mod(coef_t u, coef_t v) {
    return barrett_reduce((dcoef_t)u * (dcoef_t)v);
}

// 順変換 (Cooley-Tukey, 自然順 -> ビット反転順)。
// ステージごとに t (バタフライ間の距離) は 2のべき乗で単調減少するため、
// 割り算/剰余の代わりにシフト(tlog)とマスクだけで添字を計算する。
static void nfwd(coef_t a[NTT_N]) {
#pragma HLS INLINE off
#pragma HLS BIND_STORAGE variable=RFWD type=ROM_1P impl=BRAM
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
            // II=1を強制するとBarrett乗算器が1サイクル(組合せ回路)に押し込まれ
            // 100MHzでタイミングが閉じない (実際にHLS合成で確認済み、
            // TROUBLESHOOTING.md参照)。IIはHLSに選ばせ、深くパイプライン化された
            // 乗算器を使わせる。UART転送(往復15〜20秒)が支配的ボトルネックなので
            // IIが多少大きくても計算時間への影響は無視できる。
#pragma HLS PIPELINE
            int i       = k >> tlog;
            int j_local = k & mask;
            int j       = (i << (tlog + 1)) + j_local;

            coef_t w  = RFWD[m + i];
            coef_t hi = mul_mod(a[j + t], w);
            coef_t lo = a[j];
            a[j]     = add_mod(lo, hi);
            a[j + t] = sub_mod(lo, hi);
        }
    }
}

// 逆変換 (Gentleman-Sande, ビット反転順 -> 自然順)。N^-1 によるスケーリングを含む。
static void ninv(coef_t a[NTT_N]) {
#pragma HLS INLINE off
#pragma HLS BIND_STORAGE variable=RINV type=ROM_1P impl=BRAM
    const int half = NTT_N >> 1;

SI:
    for (int s = 0; s < NTT_LOG2N; s++) {
#pragma HLS LOOP_TRIPCOUNT min=13 max=13
        int mask = (1 << s) - 1;
        int h    = NTT_N >> (s + 1);
        int t    = 1 << s;

    BI:
        for (int k = 0; k < half; k++) {
            // BFと同じ理由でIIをHLSに選ばせる (上のコメント参照)。
#pragma HLS PIPELINE
            int i       = k >> s;
            int j_local = k & mask;
            int j       = (i << (s + 1)) + j_local;

            coef_t w  = RINV[h + i];
            coef_t lo = a[j];
            coef_t hi = a[j + t];
            a[j]     = add_mod(lo, hi);
            a[j + t] = mul_mod(sub_mod(lo, hi), w);
        }
    }

SC:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        a[i] = mul_mod(a[i], (coef_t)NTT_NINV);
    }
}

// トップ関数: mem[0..N-1]=多項式A, mem[N..2N-1]=多項式B を読み込み、
// 負巡回積 (正変換NTT x2 -> 点ごとの乗算 -> 逆変換NTT) を mem[0..N-1] に書き戻す。
// MicroBlazeがUARTからのバルク受信で mem を埋め、Start()後に IsDone() をポーリングし、
// mem からバルク送信で結果を取り出す (software/src/main.c 参照)。
void poly_mul_ntt_n8192(coef_t *mem) {
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mem    bundle=CTRL
#pragma HLS INTERFACE m_axi     port=mem    bundle=gmem depth=16384 \
        max_read_burst_length=256 max_write_burst_length=256

    coef_t A[NTT_N];
    coef_t B[NTT_N];
#pragma HLS BIND_STORAGE variable=A type=RAM_2P impl=BRAM
#pragma HLS BIND_STORAGE variable=B type=RAM_2P impl=BRAM

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

    nfwd(A);
    nfwd(B);

PW:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        A[i] = mul_mod(A[i], B[i]);
    }

    ninv(A);

WR:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        mem[i] = A[i];
    }
}
