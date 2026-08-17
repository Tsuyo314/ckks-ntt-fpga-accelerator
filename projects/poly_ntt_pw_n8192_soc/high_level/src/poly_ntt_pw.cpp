#include "ap_int.h"

// poly_ntt_pw — poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ2 (pointwise乗算)。
//
// [3フェーズ分割の背景] poly_mul_ntt_crt_n8192_soc は「順変換+pointwise乗算」と
// 「逆変換」の2フェーズを1つのビットストリームに同居させており、チップ全体で
// Block RAM 133/135 RAMB36 (98.5%) というほぼ余裕のない状態になっていた
// (poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md 5節参照)。本プロジェクトでは
// 「順変換」「pointwise乗算」「逆変換」を3つの独立したビットストリームに完全分離し、
// 各ビットストリームがそのフェーズに必要なロジック・BRAMだけを持つようにする。
// フェーズ間の受け渡しはPCが仲介する (host/host_pw.py 参照)。
//
// このカーネルはNTTドメインで既に順変換済みの2つの多項式 Â, B̂ を要素ごとに
// Barrett還元で乗算するだけで、NTTバタフライロジック・回転因子テーブル(TAB/TABSH)は
// 一切持たない。両オペランドとも実行時変数なのでShoup乗算の前提
// (乗数の片方が既知の定数) が成立せず、Barrett還元を使う
// (poly_mul_ntt_crt_n8192_soc/high_level/src/poly_mul_ntt.cpp 冒頭コメント参照。
// barrett_reduce/mul_mod_barrettはそこからの複製。バグ修正時はそちらとも突き合わせること)。
//
// mem レイアウト: mem[0,N)=Â(入力), mem[N,2N)=B̂(入力)。結果は mem[0,N) に上書きする
// (2ブロック=128KBのみ必要。順変換版の4ブロック=256KBの半分)。

typedef ap_uint<64>  coef_t;   // 60ビットのQを収める64ビットコンテナ
typedef ap_uint<128> dcoef_t;  // u*v (< Q^2, <=120ビット) を保持する2倍幅コンテナ

#define NTT_N      8192
#define NTT_W_BITS 60   // 全チャネル共通 (poly_mul_ntt_crt_n8192_soc/TROUBLESHOOTING.md 3節参照)

// Barrett還元: x (< q^2, <=120ビット) を mod q に還元する。qhatは真の商より最大1
// 小さい場合があるため、減算後に最大2回の補正が必要。
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

// トップ関数: N語ぶんのオンチップ配列(static BRAM)を一切持たない。m_axiから読んだ
// Â[i], B̂[i] をその場でBarrett乗算し、結果を同じアドレス(mem[i])へ書き戻すだけの
// 単純なストリーミングループ。
void poly_ntt_pw(coef_t *mem, coef_t q, coef_t mu) {
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mem    bundle=CTRL
#pragma HLS INTERFACE s_axilite port=q      bundle=CTRL
#pragma HLS INTERFACE s_axilite port=mu     bundle=CTRL
#pragma HLS INTERFACE m_axi     port=mem    bundle=gmem depth=16384 \
        max_read_burst_length=16 max_write_burst_length=16

PW:
    for (int i = 0; i < NTT_N; i++) {
#pragma HLS PIPELINE II=1
        coef_t a_hat = mem[i];
        coef_t b_hat = mem[NTT_N + i];
        mem[i] = mul_mod_barrett(a_hat, b_hat, q, mu);
    }
}
