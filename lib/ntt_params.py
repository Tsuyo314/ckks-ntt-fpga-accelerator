#!/usr/bin/env python3
"""NTT向きモジュラス/回転因子パラメータ生成ツール (fpga配下の各プロジェクト共有)。

リング次数 N (2のべき乗) と目標ビット長を与えると、
  - Q = k*2N + 1 の形の素数 Q を探索 (Z_Q上に原始2N乗根が存在する条件)
  - Q の原始根 g、そこから原始2N乗根 psi = g^((Q-1)/2N) mod Q を計算
  - psi^-1, N^-1 mod Q, Barrett還元定数 mu = floor(2^(2W)/Q) (W = Qのビット長)
  - ビット反転順の順変換/逆変換回転因子テーブル
    (Table[bitrev(i)] = psi^i, projects/poly_mul_ntt_openfhe_soc/NTT_ALGORITHM.md
     と同じ規約)
を求め、JSON と HLS 用 C ヘッダとして出力する。

実行例:
    python ntt_params.py --n 8192 --bits 60 \
        --out-json generated/ntt_params_n8192_q60.json \
        --out-header generated/ntt_params_n8192.h
"""
import argparse
import json
import math
import random

_MR_BASES = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]


def is_probable_prime(n: int) -> bool:
    """決定的Miller-Rabin。底 [2..37] は n < 3.3*10^24 で確定的に正しいと
    知られており、本ツールが扱う <=61ビットの候補には十分。"""
    if n < 2:
        return False
    for p in _MR_BASES:
        if n == p:
            return True
        if n % p == 0:
            return False
    d = n - 1
    r = 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for a in _MR_BASES:
        x = pow(a, d, n)
        if x == 1 or x == n - 1:
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def find_ntt_primes(n: int, bits: int, count: int) -> list:
    """Q = k*2n+1 が素数となる k を、大きい方から降順に `count` 個見つける
    (k を単調に減らしながら探すため、見つかる Q は自動的にすべて相異なる)。
    戻り値は [(Q0,k0), (Q1,k1), ...] (Q0 が目標ビット長に最も近い)。
    CRT/RNS分解で複数のNTT向き素数が必要な場合に使う。"""
    two_n = 2 * n
    k = ((1 << bits) - 1) // two_n
    floor_bits = bits - 4  # ここまで下がったら諦める (通常は素数密度的に発生しない)
    found = []
    while len(found) < count:
        q = k * two_n + 1
        if q.bit_length() < floor_bits:
            raise ValueError(
                f"only found {len(found)}/{count} {bits}-bit NTT primes for n={n}"
            )
        if is_probable_prime(q):
            found.append((q, k))
        k -= 1
    return found


def find_ntt_prime(n: int, bits: int) -> tuple:
    """Q = k*2n+1 が素数となる最大の k をビット長 `bits` 以下で探索する
    (大きい k から降順に走査するため、目標ビット長にできるだけ近い Q が
    見つかる)。戻り値は (Q, k)。"""
    return find_ntt_primes(n, bits, 1)[0]


def _pollard_rho(n: int) -> int:
    if n % 2 == 0:
        return 2
    x = random.randrange(2, n)
    y = x
    c = random.randrange(1, n)
    d = 1
    while d == 1:
        x = (x * x + c) % n
        y = (y * y + c) % n
        y = (y * y + c) % n
        d = math.gcd(abs(x - y), n)
    return d if d != n else _pollard_rho(n)


def factorize(n: int) -> dict:
    """試し割り (10^5未満) + Pollard-rho で n を素因数分解する。"""
    factors: dict = {}
    remaining = n
    p = 2
    while p * p <= remaining and p < 100_000:
        while remaining % p == 0:
            factors[p] = factors.get(p, 0) + 1
            remaining //= p
        p += 1 if p == 2 else 2
    stack = [remaining]
    while stack:
        m = stack.pop()
        if m == 1:
            continue
        if is_probable_prime(m):
            factors[m] = factors.get(m, 0) + 1
            continue
        d = _pollard_rho(m)
        stack.append(d)
        stack.append(m // d)
    return factors


def find_primitive_root(q: int, qm1_factors: dict) -> int:
    qm1 = q - 1
    g = 2
    while True:
        if all(pow(g, qm1 // p, q) != 1 for p in qm1_factors):
            return g
        g += 1


def find_primitive_2nth_root(q: int, n: int, g: int) -> int:
    two_n = 2 * n
    assert (q - 1) % two_n == 0
    psi = pow(g, (q - 1) // two_n, q)
    assert pow(psi, two_n, q) == 1, "psi の位数が 2N ではない"
    assert pow(psi, n, q) == q - 1, "psi^N が -1 ではない (2N乗根ではなくN乗根になっている)"
    return psi


def bitrev(x: int, bits: int) -> int:
    r = 0
    for _ in range(bits):
        r = (r << 1) | (x & 1)
        x >>= 1
    return r


def build_twiddle_tables(q: int, psi: int, n: int) -> tuple:
    """Table[bitrev(i)] = psi^i (順変換), psi^-i (逆変換) の規約でテーブルを作る。"""
    bits = n.bit_length() - 1
    assert (1 << bits) == n, "n は2のべき乗である必要がある"
    psi_inv = pow(psi, q - 2, q)
    fwd = [0] * n
    inv = [0] * n
    p_fwd = 1
    p_inv = 1
    for i in range(n):
        j = bitrev(i, bits)
        fwd[j] = p_fwd
        inv[j] = p_inv
        p_fwd = p_fwd * psi % q
        p_inv = p_inv * psi_inv % q
    return fwd, inv


def barrett_mu(q: int) -> int:
    w = q.bit_length()
    return (1 << (2 * w)) // q


def shoup_precompute(y: int, q: int, w_bits: int = 64) -> int:
    """Shoup乗算の事前計算定数: floor(y * 2^w_bits / q)。

    固定の乗数 y (回転因子やN^-1) について事前にこの定数 y' を求めておくと、
    実行時の x*y mod q (xは変数) は
        qhat = high_bits(x * y', w_bits)   (上位 w_bits ビット = 商の見積り)
        r    = (x*y - qhat*q) mod 2^w_bits  (w_bitsビット幅の折り返し演算)
        if r >= q: r -= q                   (補正は最大1回、Barrettの最大2回より少ない)
    で還元できる。前提: q < 2^(w_bits-1) (符号なしオーバーフローを避けるため)。
    """
    assert q < (1 << (w_bits - 1)), "Shoup前提: q は w_bits-1 ビット未満である必要がある"
    return (y * (1 << w_bits)) // q


def montgomery_precompute(q: int, r_bits: int = 64) -> dict:
    """Montgomery簡約用の定数一式 (R=2^r_bits を法とする)。

    前提: q は奇数 (2^r_bitsと互いに素) であること。本ツールが生成するQは
    Q=k*2n+1 (2nは偶数) の形なので常に奇数であり、この前提は自動的に満たされる。
    さらに q < R (=2^r_bits) であることも必要 (本ツールの60ビットQ・r_bits=64では
    自明に満たされる)。

    戻り値:
      q_inv = Q' = -Q^-1 mod R  (REDCで使うMontgomery定数)
      r2    = R^2 mod Q          (通常表現→Montgomery形式への変換 x*R mod Q =
                                   REDC(x * r2) を計算するのに使う)
    """
    r = 1 << r_bits
    assert q % 2 == 1, "Montgomery前提: qは奇数である必要がある"
    assert q < r, "Montgomery前提: q < R (=2^r_bits) である必要がある"
    q_inv = (-pow(q, -1, r)) % r
    assert (q * q_inv + 1) % r == 0, "健全性チェック: Q*Q' ≡ -1 (mod R) が成立しない"
    r2 = (r * r) % q
    return {"r_bits": r_bits, "mont_qinv": q_inv, "mont_r2": r2}


def _generate_params_for_prime(n: int, q: int, k: int, w_bits_shoup: int = 64) -> dict:
    qm1_factors = factorize(q - 1)
    g = find_primitive_root(q, qm1_factors)
    psi = find_primitive_2nth_root(q, n, g)
    psi_inv = pow(psi, q - 2, q)
    n_inv = pow(n, q - 2, q)
    w_bits = q.bit_length()
    mu = barrett_mu(q)
    fwd, inv = build_twiddle_tables(q, psi, n)

    # 追加の健全性チェック
    assert psi * psi_inv % q == 1
    assert n * n_inv % q == 1

    fwd_shoup = [shoup_precompute(v, q, w_bits_shoup) for v in fwd]
    inv_shoup = [shoup_precompute(v, q, w_bits_shoup) for v in inv]
    n_inv_shoup = shoup_precompute(n_inv, q, w_bits_shoup)

    mont = montgomery_precompute(q)
    r = 1 << mont["r_bits"]
    fwd_mont = [(v * r) % q for v in fwd]
    inv_mont = [(v * r) % q for v in inv]

    return {
        "n": n,
        "log2n": n.bit_length() - 1,
        "q": q,
        "k": k,
        "w_bits": w_bits,
        "mu": mu,
        "g": g,
        "psi": psi,
        "psi_inv": psi_inv,
        "n_inv": n_inv,
        "root_fwd": fwd,
        "root_inv": inv,
        "w_bits_shoup": w_bits_shoup,
        "root_fwd_shoup": fwd_shoup,
        "root_inv_shoup": inv_shoup,
        "n_inv_shoup": n_inv_shoup,
        "r_bits": mont["r_bits"],
        "mont_qinv": mont["mont_qinv"],
        "mont_r2": mont["mont_r2"],
        "root_fwd_mont": fwd_mont,
        "root_inv_mont": inv_mont,
    }


def generate_params(n: int, bits: int) -> dict:
    q, k = find_ntt_prime(n, bits)
    return _generate_params_for_prime(n, q, k)


def generate_params_for_q(n: int, q: int, k: int) -> dict:
    """`find_ntt_prime`の探索を経由せず、既知の (Q, k) から単一モジュラスの
    パラメータ一式を生成する。`Q = k*2n+1`であることは呼び出し側が保証すること
    (`_generate_params_for_prime`内のpsi/primitive root探索が前提とする)。

    `find_ntt_prime`は「目標ビット長に収まる最大のkから降順に探索し、最初に
    見つかった素数を採用する」ため、結果のQは必然的に2^bitsのごく近傍に偏り、
    ビットパターンも偏りがちになる (例: N=8192,bits=60ではQ=2^60-2^14+1という
    46ビット連続1の特殊な形になった)。mod還元アルゴリズムの公平な比較など、
    ビットパターンに意図的な偏りがないQが必要な場合はこの関数を使う。"""
    assert q == k * 2 * n + 1, "q は k*2n+1 の形である必要がある"
    return _generate_params_for_prime(n, q, k)


def generate_params_multi(n: int, bits: int, count: int) -> dict:
    """CRT/RNS分解用に `count` 個の相異なるNTT向き素数のパラメータをまとめて生成する。
    各チャネル (素数) は `_generate_params_for_prime` と同じ内容を持つ。
    Q_total = 全チャネルの素数の積 (RNS表現全体で表せる法)。"""
    primes = find_ntt_primes(n, bits, count)
    channels = [_generate_params_for_prime(n, q, k) for q, k in primes]
    q_total = 1
    for ch in channels:
        q_total *= ch["q"]
    return {
        "n": n,
        "log2n": n.bit_length() - 1,
        "count": count,
        "q_total": q_total,
        "channels": channels,
    }


def write_json(params: dict, path: str) -> None:
    with open(path, "w") as f:
        json.dump(params, f, indent=1)


def write_c_header(params: dict, path: str) -> None:
    n = params["n"]
    guard = f"NTT_PARAMS_N{n}_H"
    lines = []
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append("// fpga/lib/ntt_params.py で生成 (手編集しないこと)")
    lines.append(f"#define NTT_N {n}")
    lines.append(f"#define NTT_LOG2N {params['log2n']}")
    lines.append(f"#define NTT_Q {params['q']}ULL")
    lines.append(f"#define NTT_MU {params['mu']}ULL")
    lines.append(f"#define NTT_W_BITS {params['w_bits']}")
    lines.append(f"#define NTT_NINV {params['n_inv']}ULL")
    lines.append("")
    fwd_vals = ", ".join(f"{v}ULL" for v in params["root_fwd"])
    inv_vals = ", ".join(f"{v}ULL" for v in params["root_inv"])
    # 配列名は短く保つ (RFWD/RINV): HLSは関数名+ループラベル+配列名を連結した
    # ファイル名でROM初期化用の.datを生成するため、長い名前はWindowsの
    # MAX_PATH(260文字)制限に抵触しやすい (実際にNTT_ROOT_FWD/INVで発生・確認済み)。
    lines.append(f"static const coef_t RFWD[NTT_N] = {{{fwd_vals}}};")
    lines.append(f"static const coef_t RINV[NTT_N] = {{{inv_vals}}};")
    lines.append("")
    # Shoup乗算用定数 (write_c_header_multi の RFWD_SH/RINV_SH/NINV_SH_TAB と同じ
    # 命名規約だが、単一モジュラス版なのでNTT_L次元のないフラットなcoef_t[NTT_N]。
    # shoup_precompute()は_generate_params_for_prime()内で既に計算済み。
    lines.append(f"#define NTT_NINV_SH {params['n_inv_shoup']}ULL")
    fwd_sh_vals = ", ".join(f"{v}ULL" for v in params["root_fwd_shoup"])
    inv_sh_vals = ", ".join(f"{v}ULL" for v in params["root_inv_shoup"])
    lines.append(f"static const coef_t RFWD_SH[NTT_N] = {{{fwd_sh_vals}}};")
    lines.append(f"static const coef_t RINV_SH[NTT_N] = {{{inv_sh_vals}}};")
    lines.append("")
    # Montgomery還元用定数。R=2^r_bits (r_bits=64, coef_tの容器幅と一致)。
    # NTT_QINV = -Q^-1 mod R、NTT_R2 = R^2 mod Q はmontgomery_precompute()が
    # 事前計算済み。RFWD_MONT/RINV_MONT は回転因子をMontgomery形式 (v*R mod Q)
    # に変換したテーブル (通常のRFWD/RINVを置き換えて使う、追加ではない)。
    lines.append(f"#define NTT_R_BITS {params['r_bits']}")
    lines.append(f"#define NTT_QINV {params['mont_qinv']}ULL")
    lines.append(f"#define NTT_R2 {params['mont_r2']}ULL")
    fwd_mont_vals = ", ".join(f"{v}ULL" for v in params["root_fwd_mont"])
    inv_mont_vals = ", ".join(f"{v}ULL" for v in params["root_inv_mont"])
    lines.append(f"static const coef_t RFWD_MONT[NTT_N] = {{{fwd_mont_vals}}};")
    lines.append(f"static const coef_t RINV_MONT[NTT_N] = {{{inv_mont_vals}}};")
    lines.append("")
    lines.append(f"#endif // {guard}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_json_multi(params: dict, path: str) -> None:
    with open(path, "w") as f:
        json.dump(params, f, indent=1)


def write_c_header_multi(params: dict, path: str) -> None:
    """CRT/RNS版のCヘッダを出力する。単一モジュラス版 (write_c_header) と違い、
    Q/MU/N^-1/回転因子テーブルはすべてチャネル数 NTT_L 個ぶんの配列になる。
    配列名は単一版と同じ理由 (Windows MAX_PATH制限) で短く保つ。"""
    n = params["n"]
    count = params["count"]
    channels_for_check = params["channels"]
    w_bits_set = {c["w_bits"] for c in channels_for_check}
    assert len(w_bits_set) == 1, (
        "全チャネルのモジュラスは同じビット長である前提 (Barrett還元のシフト量を "
        "共通のNTT_W_BITSとして1つだけ生成するため)。異なるビット長が混在する場合は "
        "write_c_header_multi をチャネルごとのW_BITSテーブル出力に変更すること: "
        f"{sorted(w_bits_set)}"
    )
    w_bits = w_bits_set.pop()

    guard = f"NTT_PARAMS_N{n}_CRT{count}_H"
    lines = []
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append("// fpga/lib/ntt_params.py --count で生成 (手編集しないこと)")
    lines.append(f"#define NTT_N {n}")
    lines.append(f"#define NTT_LOG2N {params['log2n']}")
    lines.append(f"#define NTT_L {count}")
    lines.append(f"#define NTT_W_BITS {w_bits}  // 全チャネル共通 (MU_TABはこのビット長で計算済み)")
    lines.append(f"// Q_total (全チャネルの素数の積, {params['q_total'].bit_length()}ビット) = {params['q_total']}")
    lines.append("")

    def scalar_arr(name: str, values: list) -> str:
        vals = ", ".join(f"{v}ULL" for v in values)
        return f"static const coef_t {name}[NTT_L] = {{{vals}}};"

    channels = params["channels"]
    lines.append(scalar_arr("Q_TAB", [c["q"] for c in channels]))
    lines.append(scalar_arr("MU_TAB", [c["mu"] for c in channels]))
    lines.append(scalar_arr("NINV_TAB", [c["n_inv"] for c in channels]))
    lines.append(scalar_arr("NINV_SH_TAB", [c["n_inv_shoup"] for c in channels]))
    lines.append("")

    def table_arr(name: str, rows: list) -> str:
        out = [f"static const coef_t {name}[NTT_L][NTT_N] = {{"]
        for row in rows:
            vals = ", ".join(f"{v}ULL" for v in row)
            out.append(f"  {{{vals}}},")
        out.append("};")
        return "\n".join(out)

    lines.append(table_arr("RFWD", [c["root_fwd"] for c in channels]))
    lines.append(table_arr("RFWD_SH", [c["root_fwd_shoup"] for c in channels]))
    lines.append(table_arr("RINV", [c["root_inv"] for c in channels]))
    lines.append(table_arr("RINV_SH", [c["root_inv_shoup"] for c in channels]))
    lines.append("")
    lines.append(f"#endif // {guard}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--n", type=int, default=8192, help="リング次数 (2のべき乗)")
    ap.add_argument("--bits", type=int, default=60, help="モジュラスQの目標ビット長")
    ap.add_argument(
        "--count", type=int, default=1,
        help="CRT/RNS分解で使う相異なるNTT向き素数の個数。1なら従来通りの単一モジュラス形式で出力する",
    )
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--out-header", required=True)
    args = ap.parse_args()

    if args.count <= 1:
        params = generate_params(args.n, args.bits)
        write_json(params, args.out_json)
        write_c_header(params, args.out_header)

        print(f"N     = {params['n']}")
        print(f"Q     = {params['q']} ({params['w_bits']} bits, k={params['k']})")
        print(f"g     = {params['g']}")
        print(f"psi   = {params['psi']}")
        print(f"N_inv = {params['n_inv']}")
        print(f"mu    = {params['mu']}")
    else:
        params = generate_params_multi(args.n, args.bits, args.count)
        write_json_multi(params, args.out_json)
        write_c_header_multi(params, args.out_header)

        print(f"N       = {params['n']}")
        print(f"count L = {params['count']}")
        print(f"Q_total = {params['q_total']} ({params['q_total'].bit_length()} bits)")
        for i, ch in enumerate(params["channels"]):
            print(f"  ch{i}: Q={ch['q']} ({ch['w_bits']} bits, k={ch['k']}) g={ch['g']} psi={ch['psi']}")

    print(f"-> {args.out_json}")
    print(f"-> {args.out_header}")


if __name__ == "__main__":
    main()
