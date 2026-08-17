// ============================================================================
// poly_mul_ntt_openfhe_n8192_soc の演算部分を、FPGA・UART・Vitis 一式なしで
// 単体の C++ プログラムとして再現したもの。処理の流れを追いやすくするため、
// high_level/src/poly_mul_ntt.cpp (Vitis HLS版, ビットシフト/マスクを使った
// インデックス計算) ではなく、fpga/lib/ntt_reference.py の素直な
// (m, t, i, j1, j2) ループ構造をそのまま C++ に移植している。
// 回転因子テーブル・パラメータは既存の生成済みヘッダをそのまま再利用する。
//
// Build:  g++ -O2 -std=c++17 -o ntt_standalone ntt_standalone.cpp
// Run:    ./ntt_standalone
//
// Processing flow:
//   0. 乱数多項式 A, B を生成し、往復変換 (INTT(NTT(A))==A) のサニティチェック
//   1. Forward NTT of A   (Cooley-Tukey, 自然順 -> ビット反転順)
//   2. Forward NTT of B   (Cooley-Tukey, 自然順 -> ビット反転順)
//   3. Pointwise multiply  A_hat[i] * B_hat[i] mod Q
//   4. Inverse NTT        (Gentleman-Sande, ビット反転順 -> 自然順, 1/N スケーリング込み)
//   5. Verify vs O(N^2) 直接負巡回畳み込み (独立したオラクル)
// ============================================================================

#include <cstdint>

using coef_t = std::uint64_t;  // ntt_params_n8192.h の include より前に定義必須
#include "../high_level/src/ntt_params_n8192.h"  // NTT_N, NTT_Q, NTT_NINV, RFWD[], RINV[]

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using Poly = std::vector<coef_t>;

// ---------------------------------------------------------------------------
// モジュラー演算ヘルパー
// NTT_Q は60ビットなので u+v, u+Q-v は uint64_t の範囲(<2^61)に収まる。
// u*v のみ 2*60=120ビットに達するため unsigned __int128 が必要。
// HLS版は FPGA のハードウェア除算器を避けるために Barrett還元を使うが、
// それは実装コストの都合であって計算結果は普通の % と数学的に同一。
// ---------------------------------------------------------------------------

static inline coef_t add_mod(coef_t u, coef_t v) {
    coef_t t = u + v;
    if (t >= NTT_Q) t -= NTT_Q;
    return t;
}

static inline coef_t sub_mod(coef_t u, coef_t v) {
    coef_t t = u + NTT_Q - v;  // Q を先に足すのでアンダーフローしない
    if (t >= NTT_Q) t -= NTT_Q;
    return t;
}

static inline coef_t mul_mod(coef_t u, coef_t v) {
    return static_cast<coef_t>((static_cast<unsigned __int128>(u) * v) % NTT_Q);
}

// ---------------------------------------------------------------------------
// Step 1/2: Forward NTT (Cooley-Tukey, 自然順 -> ビット反転順)
// fpga/lib/ntt_reference.py の ntt_forward_ct をそのまま移植。
// ---------------------------------------------------------------------------
static void ntt_forward_ct(Poly &a) {
    int t = NTT_N;
    int m = 1;
    while (m < NTT_N) {
        t /= 2;
        for (int i = 0; i < m; i++) {
            coef_t w = RFWD[m + i];
            int j1 = 2 * i * t;
            int j2 = j1 + t;
            for (int j = j1; j < j2; j++) {
                coef_t hi = mul_mod(a[j + t], w);
                coef_t lo = a[j];
                a[j]     = add_mod(lo, hi);
                a[j + t] = sub_mod(lo, hi);
            }
        }
        m *= 2;
    }
}

// ---------------------------------------------------------------------------
// Step 4: Inverse NTT (Gentleman-Sande, ビット反転順 -> 自然順)
// N^-1 によるスケーリングを含む。ntt_reference.py の ntt_inverse_gs を移植。
// ---------------------------------------------------------------------------
static void ntt_inverse_gs(Poly &a) {
    int t = 1;
    int m = NTT_N;
    while (m > 1) {
        int h = m / 2;
        int j1 = 0;
        for (int i = 0; i < h; i++) {
            int j2 = j1 + t;
            coef_t w = RINV[h + i];
            for (int j = j1; j < j2; j++) {
                coef_t lo = a[j];
                coef_t hi = a[j + t];
                a[j]     = add_mod(lo, hi);
                a[j + t] = mul_mod(sub_mod(lo, hi), w);
            }
            j1 += 2 * t;
        }
        t *= 2;
        m /= 2;
    }
    for (int i = 0; i < NTT_N; i++) {
        a[i] = mul_mod(a[i], static_cast<coef_t>(NTT_NINV));
    }
}

// ---------------------------------------------------------------------------
// Step 5 の検証用: O(N^2) 直接負巡回畳み込み (独立したオラクル)
// ntt_reference.py の negacyclic_conv_direct を移植。
// ---------------------------------------------------------------------------
static Poly negacyclic_conv_direct(const Poly &a, const Poly &b) {
    Poly conv(2 * NTT_N - 1, 0);
    for (int i = 0; i < NTT_N; i++) {
        for (int j = 0; j < NTT_N; j++) {
            unsigned __int128 t = static_cast<unsigned __int128>(conv[i + j]) +
                                   static_cast<unsigned __int128>(a[i]) * b[j];
            conv[i + j] = static_cast<coef_t>(t % NTT_Q);
        }
    }
    Poly out(NTT_N);
    for (int k = 0; k < NTT_N; k++) out[k] = conv[k];
    for (int k = NTT_N; k < 2 * NTT_N - 1; k++)
        out[k - NTT_N] = sub_mod(out[k - NTT_N], conv[k]);
    return out;
}

// ---------------------------------------------------------------------------
// 出力ヘルパー
// ---------------------------------------------------------------------------
static constexpr std::array<int, 5> kSampleIdx = {0, 1, 2, NTT_N / 2, NTT_N - 1};

static void print_sample(const char *label, const Poly &p) {
    std::cout << "    " << label << ": ";
    for (int idx : kSampleIdx) {
        std::cout << "[" << idx << "]=" << p[idx] << " ";
    }
    std::cout << "\n";
}

static Poly random_poly(std::mt19937_64 &rng) {
    std::uniform_int_distribution<coef_t> dist(0, NTT_Q - 1);
    Poly p(NTT_N);
    for (auto &x : p) x = dist(rng);
    return p;
}

int main() {
    std::cout << "=== NTT standalone reproduction (N=" << NTT_N << ", Q=" << NTT_Q << ") ===\n";

    const std::uint64_t seed = 42;
    std::cout << "Seed = " << seed << "\n";
    std::mt19937_64 rng(seed);

    Poly A = random_poly(rng);
    Poly B = random_poly(rng);
    Poly A0 = A;  // 末尾のオラクル比較用に原本を保持 (順変換はin-place)
    Poly B0 = B;
    print_sample("A", A);
    print_sample("B", B);

    std::cout << "\n--- Sanity check: NTT round-trip identity (INTT(NTT(A)) == A) ---\n";
    {
        Poly rt = A;
        ntt_forward_ct(rt);
        ntt_inverse_gs(rt);
        bool ok = (rt == A);
        std::cout << "  round-trip: " << (ok ? "PASS" : "FAIL") << "\n";
        if (!ok) {
            std::cerr << "round-trip identity failed, aborting.\n";
            return 1;
        }
    }

    std::cout << "\n[1/5] Forward NTT of A  (natural order -> bit-reversed order)\n";
    ntt_forward_ct(A);
    print_sample("A_hat", A);

    std::cout << "[2/5] Forward NTT of B  (natural order -> bit-reversed order)\n";
    ntt_forward_ct(B);
    print_sample("B_hat", B);

    std::cout << "[3/5] Pointwise multiply  C_hat[i] = A_hat[i] * B_hat[i] mod Q\n";
    Poly C(NTT_N);
    for (int i = 0; i < NTT_N; i++) C[i] = mul_mod(A[i], B[i]);
    print_sample("C_hat", C);

    std::cout << "[4/5] Inverse NTT  (bit-reversed order -> natural order, includes N^-1 scaling)\n";
    auto ntt_start = std::chrono::steady_clock::now();
    ntt_inverse_gs(C);
    auto ntt_end = std::chrono::steady_clock::now();
    print_sample("result", C);

    std::cout << "[5/5] Verifying against direct O(N^2) negacyclic convolution oracle...\n";
    std::cout << "  (direct convolution takes a moment: ~" << (long long)NTT_N * NTT_N / 1000000
              << "M modmuls)\n";
    auto direct_start = std::chrono::steady_clock::now();
    Poly C_direct = negacyclic_conv_direct(A0, B0);
    auto direct_end = std::chrono::steady_clock::now();

    double ntt_elapsed =
        std::chrono::duration<double>(ntt_end - ntt_start).count();
    double direct_elapsed =
        std::chrono::duration<double>(direct_end - direct_start).count();
    std::cout << "  NTT-based (forward x2 + pointwise + inverse) elapsed: " << ntt_elapsed << " s\n";
    std::cout << "  direct convolution elapsed: " << direct_elapsed << " s\n";

    int mismatches = 0;
    int first_bad = -1;
    for (int i = 0; i < NTT_N; i++) {
        if (C[i] != C_direct[i]) {
            mismatches++;
            if (first_bad < 0) first_bad = i;
        }
    }

    if (mismatches == 0) {
        std::cout << "  result: MATCH (" << NTT_N << "/" << NTT_N << " coefficients equal)\n";
        std::cout << "\nAll checks passed.\n";
        return 0;
    } else {
        std::cout << "  result: MISMATCH (" << mismatches << "/" << NTT_N << " coefficients differ)\n";
        std::cout << "  first mismatch at index " << first_bad << ": NTT=" << C[first_bad]
                  << " direct=" << C_direct[first_bad] << "\n";
        return 1;
    }
}
