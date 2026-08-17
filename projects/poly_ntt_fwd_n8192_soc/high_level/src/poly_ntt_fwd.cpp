#include "ap_int.h"

// poly_ntt_fwd — poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ1 (順変換)。
//
// [3フェーズ分割の背景] poly_ntt_pw_n8192_soc/high_level/src/poly_ntt_pw.cpp 冒頭コメント
// および poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md 5節参照。
//
// このカーネルは A, B の両方に対して順変換 (Cooley-Tukey, 自然順 -> ビット反転順) を
// 行い、両方とも mem に書き戻す。pointwise乗算はこのビットストリームには含まれない
// (poly_ntt_pw_n8192_soc が別ビットストリームとして担当する)。回転因子乗算は
// すべて固定定数(RFWD/RFWD_SH)との積なのでShoup乗算のみを使い、Barrett還元
// (mu) は一切不要 — poly_mul_ntt_crt_n8192_soc/high_level/src/poly_mul_ntt.cpp
// 冒頭コメント参照。add_mod/sub_mod/mul_mod_shoup/ntt_fwd()はそこからの複製
// (verbatim)。バグ修正時はそちらとも突き合わせること。
//
// mem レイアウト: mem[0,N)=A(入出力), mem[N,2N)=B(入出力), mem[2N,3N)=TAB(=RFWD入力),
// mem[3N,4N)=TABSH(=RFWD_SH入力)。4ブロック=256KB。
//
// 元コードと異なり Start() をまたいだ状態保持 (static配列によるcall間persistence) は
// 不要 — このビットストリーム内に消費側フェーズ(pointwise/逆変換)が存在しないため、
// 結果は毎回 mem に書き戻して完結する。

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

// Shoup乗算: 固定の乗数 w (回転因子) について事前計算した wsh = floor(w*2^64/q) を使い、
// 実行時変数 u との積 u*w mod q を求める。前提: q < 2^63 (60ビット素数なので満たす)。
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

// 順変換 (Cooley-Tukey, 自然順 -> ビット反転順)。tab/tabshは呼び出し側 (トップ関数) が
// mem からロード済みの回転因子/Shoup係数テーブル (RFWD/RFWD_SH)。
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

void poly_ntt_fwd(coef_t *mem, coef_t q) {
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mem    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=q      bundle=CTRL
#pragma HLS INTERFACE m_axi     port=mem    bundle=gmem depth=32768 \
        max_read_burst_length=16 max_write_burst_length=16

    static coef_t A[NTT_N];
    static coef_t B[NTT_N];
    static coef_t TAB[NTT_N];
    static coef_t TABSH[NTT_N];
#pragma HLS BIND_STORAGE variable=A     type=RAM_2P impl=BRAM
#pragma HLS BIND_STORAGE variable=B     type=RAM_2P impl=BRAM
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

WA:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        mem[i] = A[i];
    }
WB:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        mem[NTT_N + i] = B[i];
    }
}
