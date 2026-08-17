#include "ap_int.h"
#include <cstdio>

// poly_ntt_fwd の csim テストベンチ。tb_data_fwd.h の実チャネル0パラメータ
// (RFWD/RFWD_SH) と、独立した経路 (fpga/lib/ntt_reference.py の ntt_forward_ct、
// HLS実装とは別言語・別実装) で計算したゴールデンA_HAT/B_HATを突き合わせる。
// tb_data_fwd.h は手編集しない (生成手順はヘッダ冒頭コメント参照)。

typedef ap_uint<64> coef_t;
#define NTT_N 8192

#include "tb_data_fwd.h"

void poly_ntt_fwd(coef_t *mem, coef_t q);

int main() {
    static coef_t mem[4 * NTT_N];

    for (int i = 0; i < NTT_N; i++) {
        mem[i]             = (unsigned long long)((i * 2654435761ULL + 1ULL) % TB_Q);  // A
        mem[NTT_N + i]     = (unsigned long long)((i * 40503ULL + 7ULL) % TB_Q);        // B
        mem[2 * NTT_N + i] = TB_RFWD[i];
        mem[3 * NTT_N + i] = TB_RFWD_SH[i];
    }

    poly_ntt_fwd(mem, (coef_t)TB_Q);

    int errors = 0;
    for (int i = 0; i < NTT_N; i++) {
        unsigned long long got_a = (unsigned long long)mem[i];
        unsigned long long got_b = (unsigned long long)mem[NTT_N + i];
        if (got_a != TB_A_HAT_EXPECTED[i]) {
            if (errors < 10) {
                printf("A_HAT MISMATCH at i=%d: got=%llu expected=%llu\n",
                       i, got_a, TB_A_HAT_EXPECTED[i]);
            }
            errors++;
        }
        if (got_b != TB_B_HAT_EXPECTED[i]) {
            if (errors < 10) {
                printf("B_HAT MISMATCH at i=%d: got=%llu expected=%llu\n",
                       i, got_b, TB_B_HAT_EXPECTED[i]);
            }
            errors++;
        }
    }

    if (errors == 0) {
        printf("TEST PASSED: %d elements OK (A_HAT + B_HAT)\n", 2 * NTT_N);
        return 0;
    }
    printf("TEST FAILED: %d mismatches\n", errors);
    return 1;
}
