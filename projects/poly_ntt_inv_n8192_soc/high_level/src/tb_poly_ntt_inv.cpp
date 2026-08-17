#include "ap_int.h"
#include <cstdio>

// poly_ntt_inv の csim テストベンチ。tb_data_inv.h の実チャネル0パラメータ
// (RINV/RINV_SH/ninv/ninvsh) と、入力 Ĉ = ntt_forward_ct(a) (fpga/lib/ntt_reference.py、
// HLS実装とは独立した経路) を使い、ntt_inverse_gs(ntt_forward_ct(a)) == a という
// round-trip性質で正しさを検証する。tb_data_inv.h は手編集しない
// (生成手順はヘッダ冒頭コメント参照)。

typedef ap_uint<64> coef_t;
#define NTT_N 8192

#include "tb_data_inv.h"

void poly_ntt_inv(coef_t *mem, coef_t q, coef_t ninv, coef_t ninvsh);

int main() {
    static coef_t mem[3 * NTT_N];

    for (int i = 0; i < NTT_N; i++) {
        mem[i]             = TB_C_HAT[i];
        mem[NTT_N + i]     = TB_RINV[i];
        mem[2 * NTT_N + i] = TB_RINV_SH[i];
    }

    poly_ntt_inv(mem, (coef_t)TB_Q, (coef_t)TB_NINV, (coef_t)TB_NINV_SH);

    int errors = 0;
    for (int i = 0; i < NTT_N; i++) {
        unsigned long long expected = (unsigned long long)((i * 2654435761ULL + 1ULL) % TB_Q);
        unsigned long long got = (unsigned long long)mem[i];
        if (got != expected) {
            if (errors < 10) {
                printf("MISMATCH at i=%d: got=%llu expected=%llu\n", i, got, expected);
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("TEST PASSED: %d elements OK\n", NTT_N);
        return 0;
    }
    printf("TEST FAILED: %d mismatches\n", errors);
    return 1;
}
