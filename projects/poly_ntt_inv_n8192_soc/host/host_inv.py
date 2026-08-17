import argparse
import json
import os
import struct
import sys
import time

import serial

# poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ3 (逆変換) を
# PCからバルク転送で叩くホストスクリプト。パイプラインの終端であり、CRT復元と
# 期待値との突き合わせもここで行う。
#
# [3フェーズ分割] poly_ntt_pw_n8192_soc/host/host_pw.py 冒頭コメント参照。
#
# 本スクリプトは host_pw.py が書き出した pw_out.json (各vector×各channelの
# pointwise乗算済み係数 Ĉ, まだNTTドメイン) を読み込み、1回のシリアルセッション内で
# 全vector×全channelをループしてフェーズ3リクエストを送信し、チャネル残差が揃った
# 時点で fpga/lib/ntt_reference.py の crt_reconstruct によりCRT復元し、
# --vectors の expected と比較して OK/MISMATCH を表示、final_result.json に書き出す。
#
# プロトコル (software/src/main.c 参照):
#   リクエスト (PC→FPGA): 'I' + ch(1B) + q(8B) + ninv(8B) + ninvsh(8B)
#     + Ĉ係数x8192(8B LE) + RINV係数x8192(8B LE) + RINV_SH係数x8192(8B LE)
#     = 1 + 1 + 24 + 3*65536 = 196634 バイト
#   レスポンス (FPGA→PC): 'i' + ch(1B) + result係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "lib"))
sys.path.insert(0, _LIB_DIR)

import ntt_reference  # noqa: E402  (fpga/lib/ntt_reference.py)

SYNC_REQ = ord('I')
SYNC_RESP = ord('i')

DEFAULT_IN = os.path.join(_LIB_DIR, "generated", "pipeline_n8192", "pw_out.json")
DEFAULT_PARAMS = os.path.join(_LIB_DIR, "generated", "ntt_params_n8192_crt2_q60.json")
DEFAULT_VECTORS = os.path.join(_LIB_DIR, "generated", "testvectors_n8192_crt2_q60.json")
DEFAULT_OUT = os.path.join(_LIB_DIR, "generated", "pipeline_n8192", "final_result.json")


def _pack_u64_list(values):
    buf = bytearray()
    for v in values:
        buf += struct.pack('<Q', v)
    return buf


def compute_channel_inv(ser, n, ch, c_hat, channel_params):
    """1チャネル分の逆変換を計算する (1往復)。戻り値は (result係数列, elapsed秒)。"""
    t0 = time.time()

    req = bytearray()
    req.append(SYNC_REQ)
    req.append(ch & 0xFF)
    req += struct.pack('<Q', channel_params["q"])
    req += struct.pack('<Q', channel_params["n_inv"])
    req += struct.pack('<Q', channel_params["n_inv_shoup"])
    req += _pack_u64_list(c_hat)
    req += _pack_u64_list(channel_params["root_inv"])
    req += _pack_u64_list(channel_params["root_inv_shoup"])
    ser.write(bytes(req))

    resp_len = 2 + n * 8
    resp = ser.read(resp_len)
    elapsed = time.time() - t0

    if len(resp) != resp_len or resp[0] != SYNC_RESP:
        raise IOError(f"unexpected response: got {len(resp)} bytes, expected {resp_len} "
                       f"(first byte={resp[0:1]!r})")
    resp_ch = resp[1]
    if resp_ch != ch:
        raise IOError(f"channel mismatch: sent ch={ch}, got ch={resp_ch}")

    data = resp[2:]
    result = [struct.unpack_from('<Q', data, 8 * i)[0] for i in range(n)]
    return result, elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM port, e.g. COM4")
    ap.add_argument("--in", dest="in_path", default=DEFAULT_IN,
                     help="host_pw.py が出力した pw_out.json")
    ap.add_argument("--params", default=DEFAULT_PARAMS,
                     help="fpga/lib/ntt_params.py --count L が出力したパラメータJSON")
    ap.add_argument("--vectors", default=DEFAULT_VECTORS,
                     help="expected係数列を含むテストベクトルJSON (correctness確認用)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="final_result.json の出力先")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    with open(args.in_path) as f:
        pw = json.load(f)
    with open(args.params) as f:
        params = json.load(f)
    with open(args.vectors) as f:
        tv = json.load(f)

    n = pw["n"]
    moduli = pw["moduli"]
    channels_params = params["channels"]
    assert [c["q"] for c in channels_params] == moduli, \
        f"pw_out ({moduli}) と params ({[c['q'] for c in channels_params]}) のモジュラスが一致しません"

    print(f"N={n}, L={pw['count_channels']}, moduli={moduli}, "
          f"{len(pw['vectors'])} vector(s) from {args.in_path}")

    out_vectors = []
    ok_count = 0
    with serial.Serial(args.port, baudrate=115200, timeout=args.timeout) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        for v in pw["vectors"]:
            idx = v["vector_index"]
            residues = [None] * len(moduli)
            elapsed_per_channel = []
            for cdata in v["channels"]:
                ch = cdata["ch"]
                result, elapsed = compute_channel_inv(
                    ser, n, ch, cdata["c_hat"], channels_params[ch])
                residues[ch] = result
                elapsed_per_channel.append(elapsed)

            out = [0] * n
            for k in range(n):
                r = [residues[i][k] for i in range(len(moduli))]
                out[k] = ntt_reference.crt_reconstruct(r, moduli)

            expected = tv["vectors"][idx]["expected"]
            match = out == expected
            ok_count += 1 if match else 0
            status = "OK" if match else "MISMATCH"
            per_ch = ", ".join(f"ch{i}={e:.2f}s" for i, e in enumerate(elapsed_per_channel))
            print(f"[{idx}] {status} ({per_ch})")
            if not match:
                mismatches = [i for i in range(n) if out[i] != expected[i]]
                print(f"      first mismatch at index {mismatches[0]}: "
                      f"fpga={out[mismatches[0]]} expected={expected[mismatches[0]]}")

            out_vectors.append({
                "vector_index": idx,
                "result": out,
                "expected": expected,
                "match": match,
            })

    print(f"Summary: {ok_count}/{len(pw['vectors'])} OK")

    final = {
        "n": n,
        "moduli": moduli,
        "count_channels": pw["count_channels"],
        "source_vectors": pw.get("source_vectors"),
        "vectors": out_vectors,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(final, f)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
