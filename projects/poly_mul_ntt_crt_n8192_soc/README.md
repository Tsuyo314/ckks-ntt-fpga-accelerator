# poly_mul_ntt_crt_n8192_soc

[poly_mul_ntt_openfhe_n8192_soc](../poly_mul_ntt_openfhe_n8192_soc/) を CRT/RNS(中国剰余定理)拡張した版。
単一の大きなモジュラスの代わりに複数の60ビットNTT素数(チャネル)で並行して剰余多項式乗算を行い、
最終結果はホスト側でCRT復元する。バタフライの回転因子乗算はBarrettではなくShoup乗算を使用。

詳細設計は [FLOW.md](FLOW.md) / [NTT_ALGORITHM.md](NTT_ALGORITHM.md)、
開発時の問題と対処は [TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## クイックスタート(ビルド済みビットストリームを使う場合)

```
# 1. ボードをUSB接続してJTAG経由で書き込み (Windows/cmd.exe)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"

# 2. PCから多項式係数を送って乗算結果を受け取る (要 pyserial)
python host/host_crt.py COM4 --count 3
```

## 通信の仕組み(概要)

FPGA側のHLSコアのBRAM予算の都合上、モジュラス・回転因子テーブルは合成時ROMに焼き込まず、
毎回ホストが送信する。1チャネルの計算は2フェーズ(順変換+点乗算 → 逆変換)・2往復のUART
通信に分割され、`host_crt.py` がチャネル数L分をループしてCRT復元まで行う
(1チャネルあたり約40秒、L=2で約80秒。詳細は `host_crt.py` 冒頭コメント参照)。

`--params` / `--vectors` のデフォルトは `../../lib/generated/ntt_params_n8192_crt2_q60.json` /
`testvectors_n8192_crt2_q60.json`。別パラメータで試したい場合は
`../../lib/ntt_params.py --count L` で再生成できる。

## ソースからフルビルドする場合

`poly_mul_ntt_openfhe_n8192_soc/README.md` と同じ手順(HLS合成→Vivadoビルド→書き込み→
MicroBlazeソフトウェア作成→実行)。HLSトップは `high_level/src/poly_mul_ntt.cpp`、
Vivadoプロジェクト名は `crt8192`。
