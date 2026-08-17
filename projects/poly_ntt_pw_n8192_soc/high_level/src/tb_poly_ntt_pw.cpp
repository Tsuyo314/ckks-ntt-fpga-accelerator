#include "ap_int.h"
#include <cstdio>

// poly_ntt_pw の csim テストベンチ。HLSのBarrett還元実装とは独立した経路
// (unsigned __int128 によるネイティブなmod演算) で期待値を計算し、突き合わせる。
// q/mu は fpga/lib/generated/ntt_params_n8192_crt2_q60.json のチャネル0の実測値。

typedef ap_uint<64> coef_t;
#define NTT_N 8192

void poly_ntt_pw(coef_t *mem, coef_t q, coef_t mu);

int main() {
    const unsigned long long q_val  = 1152921504606830593ULL;  // channel 0 (60-bit prime)
    const unsigned long long mu_val = 1152921504606863359ULL;  // floor(2^120 / q)

    static coef_t mem[2 * NTT_N];
    unsigned long long a[NTT_N], b[NTT_N];
    for (int i = 0; i < NTT_N; i++) {
        a[i] = (unsigned long long)((i * 2654435761ULL + 1ULL) % q_val);
        b[i] = (unsigned long long)((i * 40503ULL + 7ULL) % q_val);
        mem[i]         = a[i];
        mem[NTT_N + i] = b[i];
    }

    poly_ntt_pw(mem, q_val, mu_val);

    int errors = 0;
    for (int i = 0; i < NTT_N; i++) {
        unsigned __int128 prod = (unsigned __int128)a[i] * (unsigned __int128)b[i];
        unsigned long long expected = (unsigned long long)(prod % q_val);
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
    printf("TEST FAILED: %d mismatches out of %d\n", errors, NTT_N);
    return 1;
}
