import argparse
import json
import os
import struct
import sys
import time

import serial

# poly_mul_ntt_crt_n8192_soc の3フェーズ分割版、フェーズ2 (pointwise乗算) を
# PCからバルク転送で叩くホストスクリプト。
#
# [3フェーズ分割] poly_mul_ntt_crt_n8192_soc は「順変換+pointwise乗算」と「逆変換」の
# 2フェーズを1つのビットストリームに同居させていたが、本プロジェクト以降の3プロジェクト
# (poly_ntt_fwd_n8192_soc / poly_ntt_pw_n8192_soc / poly_ntt_inv_n8192_soc) では
# 「順変換」「pointwise乗算」「逆変換」を物理的に別々のビットストリームに分割し、各
# ビットストリームはそのフェーズに必要なロジック・BRAMだけを持つ。フェーズ間の
# 受け渡しはPC側のJSONファイルで行う (fpga/lib/generated/pipeline_n8192/ 配下)。
#
# 本スクリプトは host_fwd.py が書き出した fwd_out.json (各vector×各channelの
# 順変換済み係数 Â/B̂) を読み込み、1回のシリアルセッション内で全vector×全channelを
# ループしてフェーズ2リクエストを送信し、結果 (pointwise乗算済み Ĉ, まだNTTドメイン)
# を pw_out.json に書き出す。再プログラムは本スクリプト実行前に1回で済む
# (チャネルごと・ベクトルごとの再プログラムは不要)。
#
# プロトコル (software/src/main.c 参照):
#   リクエスト (PC→FPGA): 'P' + ch(1B) + q(8B) + mu(8B)
#     + Â係数x8192(8B LE) + B̂係数x8192(8B LE) = 1 + 1 + 16 + 2*65536 = 131090 バイト
#   レスポンス (FPGA→PC): 'p' + ch(1B) + Ĉ係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "lib"))
sys.path.insert(0, _LIB_DIR)

SYNC_REQ = ord('P')
SYNC_RESP = ord('p')

DEFAULT_IN = os.path.join(_LIB_DIR, "generated", "pipeline_n8192", "fwd_out.json")
DEFAULT_PARAMS = os.path.join(_LIB_DIR, "generated", "ntt_params_n8192_crt2_q60.json")
DEFAULT_OUT = os.path.join(_LIB_DIR, "generated", "pipeline_n8192", "pw_out.json")


def _pack_u64_list(values):
    buf = bytearray()
    for v in values:
        buf += struct.pack('<Q', v)
    return buf


def compute_channel_pw(ser, n, ch, a_hat, b_hat, q, mu):
    """1チャネル分のpointwise乗算を計算する (1往復)。戻り値は (Ĉ係数列, elapsed秒)。"""
    t0 = time.time()

    req = bytearray()
    req.append(SYNC_REQ)
    req.append(ch & 0xFF)
    req += struct.pack('<Q', q)
    req += struct.pack('<Q', mu)
    req += _pack_u64_list(a_hat)
    req += _pack_u64_list(b_hat)
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
                     help="host_fwd.py が出力した fwd_out.json")
    ap.add_argument("--params", default=DEFAULT_PARAMS,
                     help="fpga/lib/ntt_params.py --count L が出力したパラメータJSON (muの取得元)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="pw_out.json の出力先")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    with open(args.in_path) as f:
        fwd = json.load(f)
    with open(args.params) as f:
        params = json.load(f)

    n = fwd["n"]
    moduli = fwd["moduli"]
    channels_params = params["channels"]
    assert [c["q"] for c in channels_params] == moduli, \
        f"fwd_out ({moduli}) と params ({[c['q'] for c in channels_params]}) のモジュラスが一致しません"

    print(f"N={n}, L={fwd['count_channels']}, moduli={moduli}, "
          f"{len(fwd['vectors'])} vector(s) from {args.in_path}")

    out_vectors = []
    with serial.Serial(args.port, baudrate=115200, timeout=args.timeout) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        for v in fwd["vectors"]:
            idx = v["vector_index"]
            out_channels = []
            elapsed_per_channel = []
            for cdata in v["channels"]:
                ch = cdata["ch"]
                q = channels_params[ch]["q"]
                mu = channels_params[ch]["mu"]
                c_hat, elapsed = compute_channel_pw(
                    ser, n, ch, cdata["a_hat"], cdata["b_hat"], q, mu)
                out_channels.append({"ch": ch, "c_hat": c_hat})
                elapsed_per_channel.append(elapsed)
            out_vectors.append({"vector_index": idx, "channels": out_channels})
            per_ch = ", ".join(f"ch{i}={e:.2f}s" for i, e in enumerate(elapsed_per_channel))
            print(f"[{idx}] pointwise done ({per_ch})")

    out = {
        "n": n,
        "moduli": moduli,
        "count_channels": fwd["count_channels"],
        "source_vectors": fwd.get("source_vectors"),
        "vectors": out_vectors,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(out, f)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
