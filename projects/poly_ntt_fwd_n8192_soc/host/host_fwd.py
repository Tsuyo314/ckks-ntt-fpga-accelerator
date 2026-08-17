import argparse
import json
import os
import struct
import sys
import time

import serial

# poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ1 (順変換) を
# PCからバルク転送で叩くホストスクリプト。パイプラインの起点であり、既存の
# testvectors/paramsを直接読む唯一のスクリプト (poly_ntt_pw_n8192_soc/host/host_pw.py,
# poly_ntt_inv_n8192_soc/host/host_inv.py はそれぞれ前段の出力JSONを読む)。
#
# [3フェーズ分割] poly_ntt_pw_n8192_soc/host/host_pw.py 冒頭コメント参照。
#
# 本スクリプトは fpga/lib/ntt_params.py --count L が出力したパラメータJSONと
# fpga/lib/ntt_testvectors.py が出力したテストベクトルJSONを読み、1回のシリアル
# セッション内で全vector×全channelをループしてRNS分解 (a mod qi, b mod qi) +
# フェーズ1リクエストを送信し、結果 (順変換済み Â/B̂) を fwd_out.json に書き出す。
#
# プロトコル (software/src/main.c 参照):
#   リクエスト (PC→FPGA): 'F' + ch(1B) + q(8B)
#     + A係数x8192(8B LE) + B係数x8192(8B LE) + RFWD係数x8192(8B LE) + RFWD_SH係数x8192(8B LE)
#     = 1 + 1 + 8 + 4*65536 = 262154 バイト
#   レスポンス (FPGA→PC): 'f' + ch(1B) + Â係数x8192(8B LE) + B̂係数x8192(8B LE)
#     = 1 + 1 + 131072 = 131074 バイト

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "lib"))
sys.path.insert(0, _LIB_DIR)

SYNC_REQ = ord('F')
SYNC_RESP = ord('f')

DEFAULT_TESTVECTORS = os.path.join(_LIB_DIR, "generated", "testvectors_n8192_crt2_q60.json")
DEFAULT_PARAMS = os.path.join(_LIB_DIR, "generated", "ntt_params_n8192_crt2_q60.json")
DEFAULT_OUT = os.path.join(_LIB_DIR, "generated", "pipeline_n8192", "fwd_out.json")


def _pack_u64_list(values):
    buf = bytearray()
    for v in values:
        buf += struct.pack('<Q', v)
    return buf


def compute_channel_fwd(ser, n, ch, a, b, channel_params):
    """1チャネル分の順変換を計算する (1往復)。戻り値は (Â係数列, B̂係数列, elapsed秒)。"""
    t0 = time.time()

    req = bytearray()
    req.append(SYNC_REQ)
    req.append(ch & 0xFF)
    req += struct.pack('<Q', channel_params["q"])
    req += _pack_u64_list(a)
    req += _pack_u64_list(b)
    req += _pack_u64_list(channel_params["root_fwd"])
    req += _pack_u64_list(channel_params["root_fwd_shoup"])
    ser.write(bytes(req))

    resp_len = 2 + 2 * n * 8
    resp = ser.read(resp_len)
    elapsed = time.time() - t0

    if len(resp) != resp_len or resp[0] != SYNC_RESP:
        raise IOError(f"unexpected response: got {len(resp)} bytes, expected {resp_len} "
                       f"(first byte={resp[0:1]!r})")
    resp_ch = resp[1]
    if resp_ch != ch:
        raise IOError(f"channel mismatch: sent ch={ch}, got ch={resp_ch}")

    data = resp[2:]
    a_hat = [struct.unpack_from('<Q', data, 8 * i)[0] for i in range(n)]
    b_hat = [struct.unpack_from('<Q', data, 8 * (n + i))[0] for i in range(n)]
    return a_hat, b_hat, elapsed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM port, e.g. COM4")
    ap.add_argument("--vectors", default=DEFAULT_TESTVECTORS,
                     help="fpga/lib/ntt_testvectors.py --params <CRT/RNS版JSON> が出力したテストベクトルJSON")
    ap.add_argument("--params", default=DEFAULT_PARAMS,
                     help="fpga/lib/ntt_params.py --count L が出力したパラメータJSON")
    ap.add_argument("--count", type=int, default=1,
                     help="実行するテストベクトル数")
    ap.add_argument("--out", default=DEFAULT_OUT, help="fwd_out.json の出力先")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    with open(args.vectors) as f:
        tv = json.load(f)
    with open(args.params) as f:
        params = json.load(f)

    n = tv["n"]
    moduli = tv["moduli"]
    vectors = tv["vectors"][:args.count]
    channels_params = params["channels"]
    assert [c["q"] for c in channels_params] == moduli, \
        f"testvectors ({moduli}) と params ({[c['q'] for c in channels_params]}) のモジュラスが一致しません"

    print(f"N={n}, L={tv['count_channels']}, moduli={moduli}, "
          f"testing {len(vectors)}/{len(tv['vectors'])} vector(s) from {args.vectors}")

    out_vectors = []
    with serial.Serial(args.port, baudrate=115200, timeout=args.timeout) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        for idx, v in enumerate(vectors):
            out_channels = []
            elapsed_per_channel = []
            for ch, q in enumerate(moduli):
                ai = [x % q for x in v["a"]]
                bi = [x % q for x in v["b"]]
                a_hat, b_hat, elapsed = compute_channel_fwd(ser, n, ch, ai, bi, channels_params[ch])
                out_channels.append({"ch": ch, "a_hat": a_hat, "b_hat": b_hat})
                elapsed_per_channel.append(elapsed)
            out_vectors.append({"vector_index": idx, "channels": out_channels})
            per_ch = ", ".join(f"ch{i}={e:.2f}s" for i, e in enumerate(elapsed_per_channel))
            print(f"[{idx}] forward NTT done ({per_ch})")

    out = {
        "n": n,
        "moduli": moduli,
        "count_channels": tv["count_channels"],
        "source_vectors": os.path.basename(args.vectors),
        "vectors": out_vectors,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(out, f)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
