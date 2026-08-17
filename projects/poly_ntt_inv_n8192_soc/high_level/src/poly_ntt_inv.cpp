#include "ap_int.h"

// poly_ntt_inv — poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ3 (逆変換)。
//
// [3フェーズ分割の背景] poly_ntt_pw_n8192_soc/high_level/src/poly_ntt_pw.cpp 冒頭コメント
// および poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md 5節参照。
//
// このカーネルはpointwise乗算済み(NTTドメイン)の係数列 Ĉ に対して逆変換
// (Gentleman-Sande, ビット反転順 -> 自然順, N^-1スケーリング込み) を行い、mem に
// 書き戻す。回転因子乗算・N^-1スケーリングはすべて固定定数(RINV/RINV_SH, ninv/ninvsh)
// との積なのでShoup乗算のみを使い、Barrett還元(mu)は不要
// (poly_mul_ntt_crt_n8192_soc/high_level/src/poly_mul_ntt.cpp 冒頭コメント参照。
// add_mod/sub_mod/mul_mod_shoup/ntt_inv()はそこからの複製 (verbatim)。バグ修正時は
// そちらとも突き合わせること)。
//
// mem レイアウト: mem[0,N)=Ĉ(入力)/result(出力), mem[N,2N)=TAB(=RINV入力),
// mem[2N,3N)=TABSH(=RINV_SH入力)。3ブロック=192KB (順変換版の4ブロック=256KBより
// 1ブロック少ない。Bブロックが不要なため)。

typedef ap_uint<64>  coef_t;   // 60ビットのQを収める64ビットコンテナ
typedef ap_uint<128> dcoef_t;  // (このカーネルでは未使用。元コードとの型定義の一貫性のため残す)

#define NTT_N      8192
#define NTT_LOG2N  13

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

// Shoup乗算: 固定の乗数 w (回転因子やN^-1) について事前計算した wsh = floor(w*2^64/q) を
// 使い、実行時変数 u との積 u*w mod q を求める。前提: q < 2^63 (60ビット素数なので満たす)。
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

// 逆変換 (Gentleman-Sande, ビット反転順 -> 自然順)。N^-1によるスケーリングを含む。
// tab/tabsh は呼び出し側 (トップ関数) が mem からロード済みの回転因子/Shoup係数テーブル
// (RINV/RINV_SH)。
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

void poly_ntt_inv(coef_t *mem, coef_t q, coef_t ninv, coef_t ninvsh) {
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mem    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=q      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ninv   bundle=CTRL
#pragma HLS INTERFACE s_axilite port=ninvsh bundle=CTRL
#pragma HLS INTERFACE m_axi     port=mem    bundle=gmem depth=24576 \
        max_read_burst_length=16 max_write_burst_length=16

    static coef_t A[NTT_N];
    static coef_t TAB[NTT_N];
    static coef_t TABSH[NTT_N];
#pragma HLS BIND_STORAGE variable=A     type=RAM_2P impl=BRAM
#pragma HLS BIND_STORAGE variable=TAB   type=RAM_1P impl=BRAM
#pragma HLS BIND_STORAGE variable=TABSH type=RAM_1P impl=BRAM

RT:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        TAB[i] = mem[NTT_N + i];
    }
RTS:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        TABSH[i] = mem[2 * NTT_N + i];
    }
RA:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        A[i] = mem[i];
    }

    ntt_inv(A, TAB, TABSH, q, ninv, ninvsh);

WR:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        mem[i] = A[i];
    }
}
