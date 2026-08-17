import argparse
import json
import os
import struct
import sys
import time

import serial

# poly_mul_ntt_crt_n8192_soc (CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT, N=8192,
# Lチャネル、各チャネル60ビット素数qi) をPCからバルク転送で叩くホストスクリプト。
#
# [2フェーズ構成] FPGA側HLSコアのBRAM予算の都合上 (poly_mul_ntt.cpp/TROUBLESHOOTING.md
# 参照)、モジュラス・回転因子テーブルは合成時ROMではなく毎回ホストが送る。1回のチャネル
# 計算は2回のUART往復に分割される:
#   フェーズ0 (順変換+点ごとの乗算): q/mu/n_inv/n_inv_shoup と A,B の残差多項式、
#     およびそのチャネルの順変換用回転因子テーブル(root_fwd/root_fwd_shoup)を送る。
#     結果はFPGA側コア内部に保持されたままなので、応答はACKのみ。
#   フェーズ1 (逆変換): 同じ共有メモリ領域を逆変換用テーブル(root_inv/root_inv_shoup)で
#     上書きして送る。FPGA側はそれを使って逆変換を行い、最終結果を返す。
#
# Lチャネル分の入力分解 (a mod qi の計算) と、Lチャネル分の結果残差からの最終的な
# CRT復元 (中国剰余定理による統合) は、本スクリプトが担当する
# (fpga/lib/ntt_reference.py の crt_reconstruct を使用)。
#
# プロトコル (software/src/main.c 参照):
#   フェーズ0リクエスト (PC→FPGA): 'C' + ch(1B) + q(8B) + mu(8B) + ninv(8B) + ninvsh(8B)
#     + A係数x8192(8B LE) + B係数x8192(8B LE) + RFWD x8192(8B LE) + RFWD_SH x8192(8B LE)
#     = 1 + 1 + 32 + 4*65536 = 262178 バイト
#   フェーズ0レスポンス (FPGA→PC): 'A' + ch(1B) = 2 バイト (ACKのみ)
#   フェーズ1リクエスト (PC→FPGA): 'D' + ch(1B) + RINV x8192(8B LE) + RINV_SH x8192(8B LE)
#     = 1 + 1 + 2*65536 = 131074 バイト
#   フェーズ1レスポンス (FPGA→PC): 'R' + ch(1B) + 結果係数x8192(8B LE) = 1 + 1 + 65536 = 65538 バイト
#
# 1チャネルあたり合計 458792 バイトを転送する。115200bps実効約11.5KB/sとして
# 概算40秒/チャネル、L=2で概算80秒。
#
# パラメータ (q/mu/n_inv/n_inv_shoup/root_fwd/root_fwd_shoup/root_inv/root_inv_shoup) は
# fpga/lib/ntt_params.py --count L が出力したJSON (--params) から、期待値は
# fpga/lib/ntt_testvectors.py が出力したテストベクトルJSON (--vectors) から読み込む。

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_LIB_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "lib"))
sys.path.insert(0, _LIB_DIR)

import ntt_reference  # noqa: E402  (fpga/lib/ntt_reference.py)

SYNC_REQ0 = ord('C')
SYNC_ACK0 = ord('A')
SYNC_REQ1 = ord('D')
SYNC_RESP = ord('R')

DEFAULT_TESTVECTORS = os.path.join(_LIB_DIR, "generated", "testvectors_n8192_crt2_q60.json")
DEFAULT_PARAMS = os.path.join(_LIB_DIR, "generated", "ntt_params_n8192_crt2_q60.json")


def _pack_u64_list(values):
    buf = bytearray()
    for v in values:
        buf += struct.pack('<Q', v)
    return buf


def compute_channel(ser, n, ch, a, b, channel_params):
    """1チャネル分の残差多項式積を計算する (フェーズ0+フェーズ1の2往復)。
    戻り値は (係数列, elapsed秒)。"""
    t0 = time.time()

    # --- フェーズ0: 順変換+点ごとの乗算 ---
    req0 = bytearray()
    req0.append(SYNC_REQ0)
    req0.append(ch & 0xFF)
    req0 += struct.pack('<Q', channel_params["q"])
    req0 += struct.pack('<Q', channel_params["mu"])
    req0 += struct.pack('<Q', channel_params["n_inv"])
    req0 += struct.pack('<Q', channel_params["n_inv_shoup"])
    req0 += _pack_u64_list(a)
    req0 += _pack_u64_list(b)
    req0 += _pack_u64_list(channel_params["root_fwd"])
    req0 += _pack_u64_list(channel_params["root_fwd_shoup"])
    ser.write(bytes(req0))

    ack = ser.read(2)
    if len(ack) != 2 or ack[0] != SYNC_ACK0:
        raise IOError(f"phase-0 ack失敗: got {len(ack)} bytes (first byte={ack[0:1]!r})")
    if ack[1] != ch:
        raise IOError(f"phase-0 channel mismatch: sent ch={ch}, got ch={ack[1]}")

    # --- フェーズ1: 逆変換 ---
    req1 = bytearray()
    req1.append(SYNC_REQ1)
    req1.append(ch & 0xFF)
    req1 += _pack_u64_list(channel_params["root_inv"])
    req1 += _pack_u64_list(channel_params["root_inv_shoup"])
    ser.write(bytes(req1))

    resp_len = 2 + n * 8
    resp = ser.read(resp_len)
    elapsed = time.time() - t0

    if len(resp) != resp_len or resp[0] != SYNC_RESP:
        raise IOError(f"unexpected phase-1 response: got {len(resp)} bytes, expected {resp_len} "
                       f"(first byte={resp[0:1]!r})")
    resp_ch = resp[1]
    if resp_ch != ch:
        raise IOError(f"phase-1 channel mismatch: sent ch={ch}, got ch={resp_ch}")

    data = resp[2:]
    result = [struct.unpack_from('<Q', data, 8 * i)[0] for i in range(n)]
    return result, elapsed


def compute_rns(ser, n, moduli, channels_params, a, b):
    """全チャネルを時分割で叩き、結果残差をCRT復元して1つの多項式係数列を返す。
    戻り値は (係数列, 全チャネル合計elapsed秒, チャネルごとのelapsed秒リスト)。"""
    residues = []
    elapsed_per_channel = []
    for ch, q in enumerate(moduli):
        ai = [x % q for x in a]
        bi = [x % q for x in b]
        result, elapsed = compute_channel(ser, n, ch, ai, bi, channels_params[ch])
        residues.append(result)
        elapsed_per_channel.append(elapsed)

    out = [0] * n
    for k in range(n):
        r = [residues[i][k] for i in range(len(moduli))]
        out[k] = ntt_reference.crt_reconstruct(r, moduli)
    return out, sum(elapsed_per_channel), elapsed_per_channel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM port, e.g. COM4")
    ap.add_argument("--vectors", default=DEFAULT_TESTVECTORS,
                     help="fpga/lib/ntt_testvectors.py --params <CRT/RNS版JSON> が出力したテストベクトルJSON")
    ap.add_argument("--params", default=DEFAULT_PARAMS,
                     help="fpga/lib/ntt_params.py --count L が出力したパラメータJSON "
                          "(チャネルごとのq/mu/n_inv/n_inv_shoup/回転因子テーブルを含む)")
    ap.add_argument("--count", type=int, default=1,
                     help="実行するテストベクトル数 (1件あたり概算80秒、L=2の場合)")
    ap.add_argument("--timeout", type=float, default=90.0)
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

    print(f"N={n}, L={tv['count_channels']}, moduli={moduli}, Q_total={tv['q_total']}, "
          f"testing {len(vectors)}/{len(tv['vectors'])} vector(s) from {args.vectors}")

    with serial.Serial(args.port, baudrate=115200, timeout=args.timeout) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        ok_count = 0
        for idx, v in enumerate(vectors):
            result, elapsed, elapsed_per_channel = compute_rns(
                ser, n, moduli, channels_params, v["a"], v["b"])
            match = result == v["expected"]
            ok_count += 1 if match else 0
            status = "OK" if match else "MISMATCH"
            per_ch = ", ".join(f"ch{i}={e:.2f}s" for i, e in enumerate(elapsed_per_channel))
            print(f"[{idx}] {status} (total={elapsed:.2f}s, {per_ch})")
            if not match:
                mismatches = [i for i in range(n) if result[i] != v["expected"][i]]
                print(f"      first mismatch at index {mismatches[0]}: "
                      f"fpga={result[mismatches[0]]} expected={v['expected'][mismatches[0]]}")

        print(f"Summary: {ok_count}/{len(vectors)} OK")


if __name__ == "__main__":
    main()
