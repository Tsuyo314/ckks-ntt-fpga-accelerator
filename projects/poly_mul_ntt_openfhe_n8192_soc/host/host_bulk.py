import argparse
import json
import os
import struct
import sys
import time

import serial

# poly_mul_ntt_openfhe_n8192_soc (OpenFHE方式 負巡回NTT, N=8192, mod Q (60ビット)) を
# PCからバルク転送で叩くホストスクリプト。
#
# プロトコル (software/src/main.c 参照):
#   リクエスト (PC→FPGA): 'C' + A係数x8192(8バイトLE) + B係数x8192(8バイトLE)
#     = 1 + 2*8192*8 = 131073 バイト
#   レスポンス (FPGA→PC): 'R' + 結果係数x8192(8バイトLE)
#     = 1 + 8192*8 = 65537 バイト
#
# 期待値は fpga/lib/ntt_testvectors.py で事前生成した共有テストベクトル
# (NTTベースの O(N log N) リファレンス実装 fpga/lib/ntt_reference.py が
# オラクル) から読み込む。115200bpsでの往復転送は概算15〜20秒かかるため、
# タイムアウトは大きめに取り、既定では少数のベクトルのみ実行する。

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "lib"))
sys.path.insert(0, _LIB_DIR)

import ntt_reference  # noqa: E402  (fpga/lib/ntt_reference.py)

SYNC_REQ = ord('C')
SYNC_RESP = ord('R')
DEFAULT_TESTVECTORS = os.path.join(_LIB_DIR, "generated", "testvectors_n8192_q60.json")


def compute(ser, n, a, b):
    req = bytearray()
    req.append(SYNC_REQ)
    for v in a:
        req += struct.pack('<Q', v)
    for v in b:
        req += struct.pack('<Q', v)

    t0 = time.time()
    ser.write(bytes(req))

    resp_len = 1 + n * 8
    resp = ser.read(resp_len)
    elapsed = time.time() - t0

    if len(resp) != resp_len or resp[0] != SYNC_RESP:
        raise IOError(f"unexpected response: got {len(resp)} bytes, expected {resp_len} "
                       f"(first byte={resp[0:1]!r})")

    data = resp[1:]
    result = [struct.unpack_from('<Q', data, 8 * i)[0] for i in range(n)]
    return result, elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM port, e.g. COM4")
    ap.add_argument("--vectors", default=DEFAULT_TESTVECTORS,
                     help="fpga/lib/ntt_testvectors.py が出力したテストベクトルJSON")
    ap.add_argument("--count", type=int, default=1,
                     help="実行するテストベクトル数 (1件あたり概算15〜20秒)")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    with open(args.vectors) as f:
        tv = json.load(f)
    n = tv["n"]
    vectors = tv["vectors"][:args.count]

    print(f"N={n}, Q={tv['q']}, testing {len(vectors)}/{len(tv['vectors'])} vector(s) "
          f"from {args.vectors}")

    with serial.Serial(args.port, baudrate=115200, timeout=args.timeout) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        ok_count = 0
        for idx, v in enumerate(vectors):
            result, elapsed = compute(ser, n, v["a"], v["b"])
            match = result == v["expected"]
            ok_count += 1 if match else 0
            status = "OK" if match else "MISMATCH"
            print(f"[{idx}] {status} (elapsed={elapsed:.2f}s)")
            if not match:
                mismatches = [i for i in range(n) if result[i] != v["expected"][i]]
                print(f"      first mismatch at index {mismatches[0]}: "
                      f"fpga={result[mismatches[0]]} expected={v['expected'][mismatches[0]]}")

        print(f"Summary: {ok_count}/{len(vectors)} OK")


if __name__ == "__main__":
    main()
