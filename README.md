# NTT-based Polynomial Multiplication Accelerator on Nexys A7-100T

FHE(準同型暗号)/OpenFHEで使われる負巡回NTT(数論変換)ベースの多項式乗算を、
Digilent Nexys A7-100T (Xilinx/AMD `xc7a100tcsg324-1`) 上に実装したFPGAアクセラレータ群。
MicroBlaze SoC + Vitis HLSで生成したNTTコアをAXIで接続し、PCからUSB-UART経由で
多項式係数を送ると、FPGA上で乗算した結果を送り返す。

いずれのプロジェクトも実機(Nexys A7-100T)で、ビットストリーム書き込み・MicroBlazeファームウェア
ダウンロード・Pythonホストスクリプトからの送受信を通しで確認済み(2026-08-17)。ビルド済み
ビットストリーム(`low_level/bitstream/*.bit`)とMicroBlazeファームウェア(`software/elf/*.elf`)を
同梱しているため、Vivado/Vitisでのリビルドなしにそのまま書き込んで試すことができる。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2(ソースからリビルドする場合)
- Python 3.8+ (`pip install -r requirements.txt`)

## 構成

| プロジェクト | 内容 |
|---|---|
| [`projects/poly_mul_ntt_openfhe_n8192_soc`](projects/poly_mul_ntt_openfhe_n8192_soc/) | N=8192、単一60ビットNTT素数、Barrett還元。順変換→点乗算→逆変換を1ビットストリームで実行する基本実装 |
| [`projects/poly_mul_ntt_crt_n8192_soc`](projects/poly_mul_ntt_crt_n8192_soc/) | 上記をCRT/RNS(中国剰余定理)拡張。複数の60ビット素数(チャネル)で並行計算し、Shoup乗算でバタフライを高速化 |
| [`projects/poly_ntt_fwd_n8192_soc`](projects/poly_ntt_fwd_n8192_soc/) | CRT版をタイミング制約解消のため3分割した **Phase 1: 順変換** |
| [`projects/poly_ntt_pw_n8192_soc`](projects/poly_ntt_pw_n8192_soc/) | 同 **Phase 2: 点ごとの乗算**(Barrett還元、テーブル不要で最小構成) |
| [`projects/poly_ntt_inv_n8192_soc`](projects/poly_ntt_inv_n8192_soc/) | 同 **Phase 3: 逆変換 + CRT復元** |

`poly_mul_ntt_openfhe_n8192_soc` / `poly_mul_ntt_crt_n8192_soc` は単体で完結した1ビットストリーム実装、
`poly_ntt_fwd/pw/inv_n8192_soc` の3つは互いに連携して動く3段パイプライン(それぞれ別ビットストリームを
順に書き込んで実行する)。詳細な実行手順は各プロジェクトの `README.md` を参照。

CRT/RNS版(`poly_mul_ntt_crt_n8192_soc` および `poly_ntt_fwd/pw/inv_n8192_soc`)は
同梱パラメータでチャネル数(60ビット素数の個数)**L=2**。この基板ではBRAM使用率が
1チャネル分だけで98.5%に達しており、チャネル数を増やすほど処理時間も線形に伸びるため、
実用上はL=2〜3程度が限度になる。詳細は
[`poly_mul_ntt_crt_n8192_soc/README.md`](projects/poly_mul_ntt_crt_n8192_soc/README.md#チャネル数l-素数の個数について)
を参照。

## 共通のアーキテクチャ

```
PC (host/*.py, USB-UART 115200bps)
   ↕
MicroBlaze (software/src/main.c) ── AXI Uartlite
   ↕ AXI4 / AXI4-Lite
共有 AXI BRAM ←→ NTTアクセラレータ (high_level/src/*.cpp, Vitis HLS生成IP)
```

各プロジェクトの Block Design(MicroBlaze + AXI Uartlite + 共有BRAM + 自作HLS IP)は
`low_level/scripts/create_project.tcl` で構築される。共通で使う数論変換パラメータ生成・
リファレンス実装・テストベクトルは [`lib/`](lib/) にまとめてある。

## クイックスタート(例: poly_mul_ntt_openfhe_n8192_soc)

```
cd projects/poly_mul_ntt_openfhe_n8192_soc

# 1. ボードをUSB接続してビルド済みビットストリームを書き込み (Windows/cmd.exe)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"

# 2. MicroBlazeにファームウェア(ELF)をダウンロードして実行
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl software\elf\poly_mul_openfhe_n8192_app.elf"

# 3. PCから多項式係数を送って乗算結果を受け取る
pip install -r ../../requirements.txt
python host/host_bulk.py COM4 --count 3
```

ソースからのフルビルド手順(HLS合成→Vivadoビルド→MicroBlazeソフトウェア作成)は
各プロジェクトの `README.md` を参照。

## ライセンス

[MIT License](LICENSE)
