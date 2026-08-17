#!/usr/bin/env python3
"""再利用可能なテストベクトル生成ツール (fpga配下の各プロジェクト共有)。

ntt_params.py で生成したパラメータJSONを読み込み、ntt_reference.py の
negacyclic_poly_mul をオラクルとしてシード付き乱数の多項式ペア(A,B)と
その期待値を複数セット生成し、JSONとして保存する。他プロジェクトの
ホストスクリプトからも同じテストベクトルを読み込んで使い回せる。

実行例:
    python ntt_testvectors.py --params generated/ntt_params_n8192_q60.json \
        --count 8 --seed 1234 --out generated/testvectors_n8192_q60.json
"""
import argparse
import json
import random

import ntt_reference


def generate_test_vectors(params: dict, count: int, seed: int) -> list:
    q = params["q"]
    n = params["n"]
    rnd = random.Random(seed)
    vectors = []
    for _ in range(count):
        a = [rnd.randrange(q) for _ in range(n)]
        b = [rnd.randrange(q) for _ in range(n)]
        expected = ntt_reference.negacyclic_poly_mul(a, b, params)
        vectors.append({"a": a, "b": b, "expected": expected})
    return vectors


def generate_test_vectors_rns(multi_params: dict, count: int, seed: int) -> list:
    """CRT/RNS版のテストベクトル生成。`ntt_params.generate_params_multi()` の
    出力 (複数チャネルパラメータ、`--count`>=2 で `ntt_params.py` が出力したJSON) を使う。
    オラクルは `ntt_reference.negacyclic_poly_mul_rns`
    (各チャネルNTT畳み込み + CRT復元。O(N^2)直接畳み込みではなくO(N logN)xチャネル数)。
    係数 a, b は Q_total = prod(qi) 未満の整数として生成する。"""
    channels = multi_params["channels"]
    q_total = multi_params["q_total"]
    n = multi_params["n"]
    rnd = random.Random(seed)
    vectors = []
    for _ in range(count):
        a = [rnd.randrange(q_total) for _ in range(n)]
        b = [rnd.randrange(q_total) for _ in range(n)]
        expected = ntt_reference.negacyclic_poly_mul_rns(a, b, channels)
        vectors.append({"a": a, "b": b, "expected": expected})
    return vectors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--params", required=True,
                     help="ntt_params.py が出力したJSON (単一素数 or --count>=2 のCRT/RNS版)")
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    params = ntt_reference.load_params(args.params)

    if "channels" in params:
        vectors = generate_test_vectors_rns(params, args.count, args.seed)
        with open(args.out, "w") as f:
            json.dump({
                "n": params["n"],
                "count_channels": params["count"],
                "moduli": [c["q"] for c in params["channels"]],
                "q_total": params["q_total"],
                "seed": args.seed,
                "vectors": vectors,
            }, f, indent=1)
        print(f"generated {len(vectors)} RNS test vectors "
              f"(n={params['n']}, L={params['count']}, Q_total={params['q_total']}, seed={args.seed})")
    else:
        vectors = generate_test_vectors(params, args.count, args.seed)
        with open(args.out, "w") as f:
            json.dump({"n": params["n"], "q": params["q"], "seed": args.seed, "vectors": vectors}, f, indent=1)
        print(f"generated {len(vectors)} test vectors (n={params['n']}, q={params['q']}, seed={args.seed})")

    print(f"-> {args.out}")


if __name__ == "__main__":
    main()
