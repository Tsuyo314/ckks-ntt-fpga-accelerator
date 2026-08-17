#!/usr/bin/env python3
"""OpenFHE方式 (CT-forward / GS-inverse, 負巡回) NTTの共有Pythonリファレンス実装。

fpga配下の各プロジェクト (poly_mul_ntt_openfhe_soc, poly_mul_ntt_openfhe_n8192_soc等)
が、ハードウェア結果の検証オラクルとして共通で import できる、使い捨てでない
O(N log N) 実装。アルゴリズムの段構成は
projects/poly_mul_ntt_openfhe_soc/high_level/src/poly_mul_ntt.cpp の
ntt4_fwd_ct/ntt4_inv_gs (N=4手展開) と完全に同じ規約 (回転因子テーブルは
ビット反転順 Table[bitrev(i)]=psi^i、線形カウンタ m+i でインデックス) を
一般のNに対してループで実装したもの。
"""
import json


def load_params(json_path: str) -> dict:
    with open(json_path) as f:
        return json.load(f)


def ntt_forward_ct(a: list, params: dict) -> list:
    """Cooley-Tukey 順変換。自然順で入力し、ビット反転順で出力する。"""
    q = params["q"]
    n = params["n"]
    root_fwd = params["root_fwd"]
    a = list(a)
    t = n
    m = 1
    while m < n:
        t //= 2
        for i in range(m):
            w = root_fwd[m + i]
            j1 = 2 * i * t
            j2 = j1 + t
            for j in range(j1, j2):
                hi = (a[j + t] * w) % q
                lo = a[j]
                a[j] = (lo + hi) % q
                a[j + t] = (lo - hi) % q
        m *= 2
    return a


def ntt_inverse_gs(a: list, params: dict) -> list:
    """Gentleman-Sande 逆変換。ビット反転順で入力し、自然順で出力する
    (N^-1 によるスケーリングを含む)。"""
    q = params["q"]
    n = params["n"]
    root_inv = params["root_inv"]
    n_inv = params["n_inv"]
    a = list(a)
    t = 1
    m = n
    while m > 1:
        h = m // 2
        j1 = 0
        for i in range(h):
            j2 = j1 + t
            w = root_inv[h + i]
            for j in range(j1, j2):
                lo = a[j]
                hi = a[j + t]
                a[j] = (lo + hi) % q
                a[j + t] = ((lo - hi) * w) % q
            j1 += 2 * t
        t *= 2
        m //= 2
    for i in range(n):
        a[i] = (a[i] * n_inv) % q
    return a


def negacyclic_poly_mul(a: list, b: list, params: dict) -> list:
    """正変換NTT x2 -> 点ごとの乗算 -> 逆変換NTT のフルラウンドトリップ。
    環 Z_Q[x]/(x^N+1) 上の負巡回多項式乗算 (ゼロ埋め不要)。"""
    q = params["q"]
    n = params["n"]
    A = ntt_forward_ct(a, params)
    B = ntt_forward_ct(b, params)
    C = [(A[i] * B[i]) % q for i in range(n)]
    return ntt_inverse_gs(C, params)


def negacyclic_conv_direct(a: list, b: list, q: int, n: int) -> list:
    """O(N^2) の直接負巡回畳み込み。NTTベース実装自体の正しさを検証するための
    クロスチェック専用 (N=8192のような大きなNでは常用のオラクルにしない)。
    q はモジュラスであれば何でもよい (単一のNTT向き素数でも、CRT復元後の
    Q_total = prod(qi) のような合成数でもよい)。"""
    conv = [0] * (2 * n - 1)
    for i in range(n):
        for j in range(n):
            conv[i + j] = (conv[i + j] + a[i] * b[j]) % q
    out = [0] * n
    for k in range(n):
        out[k] = conv[k] % q
    for k in range(n, 2 * n - 1):
        out[k - n] = (out[k - n] - conv[k]) % q
    return out


def crt_reconstruct(residues: list, moduli: list) -> int:
    """中国剰余定理(CRT)による復元: r_i = x mod moduli[i] (全i) から
    x mod prod(moduli) を求める。moduli は互いに素であること
    (ntt_params.find_ntt_primes()が返す素数はすべて相異なるため自動的に満たす)。
    poly_mul_ntt_crt_n8192_soc のホスト側で、FPGAが返すチャネルごとの残差多項式を
    最終的な係数へ統合するのに使う。"""
    q_total = 1
    for q in moduli:
        q_total *= q
    x = 0
    for r, q in zip(residues, moduli):
        m = q_total // q
        m_inv = pow(m, -1, q)
        x += r * m * m_inv
    return x % q_total


def negacyclic_poly_mul_rns(a: list, b: list, channels: list) -> list:
    """CRT/RNS分解による負巡回多項式乗算 (Pythonリファレンス)。

    `channels` は ntt_params.generate_params_multi(...)['channels'] と同じ形式
    (各要素が独立したNTT向き素数 qi に対応するパラメータ辞書) のリスト。
    入力係数 a, b は Q_total = prod(qi) 未満の整数と仮定し、
      1. 各チャネルで ai = a mod qi, bi = b mod qi へ分解 (RNS分解)
      2. チャネルごとに独立して negacyclic_poly_mul (NTTベース負巡回積) を計算
      3. 係数ごとに crt_reconstruct でチャネルの残差を統合し、Q_total未満の整数を得る
    という、poly_mul_ntt_crt_n8192_soc のハードウェア実装 (FPGA側は1,2を担当、
    ホスト側 (Python) が3を担当) と同じ手順を踏む。"""
    n = channels[0]["n"]
    moduli = [c["q"] for c in channels]
    per_channel_results = []
    for c in channels:
        q = c["q"]
        ai = [x % q for x in a]
        bi = [x % q for x in b]
        per_channel_results.append(negacyclic_poly_mul(ai, bi, c))
    out = [0] * n
    for k in range(n):
        residues = [per_channel_results[i][k] for i in range(len(channels))]
        out[k] = crt_reconstruct(residues, moduli)
    return out


def _self_test():
    import random
    import ntt_params

    for n, bits in [(4, 12), (8, 16), (16, 20)]:
        params = ntt_params.generate_params(n, bits)
        q = params["q"]
        rnd = random.Random(1234 + n)

        # 1) 正変換 -> 逆変換が恒等写像になること
        for _ in range(20):
            a = [rnd.randrange(q) for _ in range(n)]
            rt = ntt_inverse_gs(ntt_forward_ct(a, params), params)
            assert rt == a, f"round-trip identity failed at n={n}"

        # 2) NTTベースの負巡回乗算がO(N^2)直接畳み込みと一致すること
        for _ in range(20):
            a = [rnd.randrange(q) for _ in range(n)]
            b = [rnd.randrange(q) for _ in range(n)]
            via_ntt = negacyclic_poly_mul(a, b, params)
            via_direct = negacyclic_conv_direct(a, b, q, n)
            assert via_ntt == via_direct, f"NTT vs direct convolution mismatch at n={n}"

        print(f"n={n} (Q={q}, {params['w_bits']} bits): OK (40 random cases)")

    print("ntt_reference self-test: all passed")


def _self_test_rns():
    """RNS分解 -> 各チャネルNTT畳み込み -> CRT復元が、Q_totalでの直接畳み込みと
    一致することを検証する (poly_mul_ntt_crt_n8192_soc の正しさの根拠)。"""
    import random
    import ntt_params

    for n, bits, count in [(4, 12, 2), (8, 16, 2), (8, 14, 3), (16, 18, 2)]:
        params = ntt_params.generate_params_multi(n, bits, count)
        channels = params["channels"]
        q_total = params["q_total"]
        rnd = random.Random(9000 + n * 10 + count)

        for _ in range(20):
            a = [rnd.randrange(q_total) for _ in range(n)]
            b = [rnd.randrange(q_total) for _ in range(n)]
            via_rns = negacyclic_poly_mul_rns(a, b, channels)
            via_direct = negacyclic_conv_direct(a, b, q_total, n)
            assert via_rns == via_direct, (
                f"RNS vs direct convolution mismatch at n={n}, count={count}"
            )

        print(f"n={n}, count={count} (Q_total={q_total}, {q_total.bit_length()} bits): OK (20 random cases)")

    print("ntt_reference RNS/CRT self-test: all passed")


if __name__ == "__main__":
    _self_test()
    _self_test_rns()
