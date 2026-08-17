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

## クイックスタート(ビルド済みビットストリーム+ファームウェアを使う場合)

ビットストリームの書き込みだけではMicroBlazeにプログラムが載っていないため応答が返らない。
`software/elf/poly_mul_crt_n8192_app.elf`(MicroBlaze側ファームウェア)のダウンロードも必須(実機検証済み)。

```
# 1. ボードをUSB接続してビットストリームを書き込み (Windows/cmd.exe)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"

# 2. MicroBlazeにファームウェア(ELF)をダウンロードして実行
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl software\elf\poly_mul_crt_n8192_app.elf"

# 3. PCから多項式係数を送って乗算結果を受け取る (要 pyserial)
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

## チャネル数(L, 素数の個数)について

同梱のパラメータ・実機検証は **L=2**(60ビット素数2個)。理論上はハードウェア側は
チャネル数に依存しない設計になっている(後述)ため `ntt_params.py --count L` で
L=3以上に増やすこと自体は可能だが、以下の理由で**この基板(xc7a100t)ではL=2〜3程度が
実用上の限度**になる:

- **BRAMがすでにほぼ上限**: 当初は「Lチャネル分の回転因子テーブルを合成時ROMとして
  同時にBRAMへ焼き込む」設計だったが、これは**L=2の時点でBRAM 332/270ブロック(122%)**
  となり、xc7a100tに収まらなかった([TROUBLESHOOTING.md](TROUBLESHOOTING.md) #5)。
  そのため「1チャネル分のテーブルだけをオンチップに保持し、チャネル切り替え時にホストが
  UART経由で都度上書きする」設計に作り直した。この結果、**チャネル数Lそのものは
  ハードウェア資源を消費しなくなった**(Lはホスト側のソフトウェアループ回数に過ぎない)。
  ただし作り直した後の最終構成でも、チップ全体のBlock RAMは**1チャネル分だけで
  133/135 RAMB36(98.5%)**に達しており、これ以上BRAMを追加で使う機能拡張(NTT_Nを
  増やす、テーブルを増やす等)の余地はほぼ残っていない。
- **時間コストが素数の数に比例して増える**: L個の素数それぞれについて、順変換+点乗算
  (約40秒)と逆変換(約40秒)の2回のUART往復が必要なので、1回の多項式乗算の所要時間は
  おおよそ L×80秒。L=2で約160秒、L=3なら約240秒と、通信(115200bps)が支配的な
  ボトルネックのため素数を増やすほど線形に遅くなる。

まとめると、**「BRAMに焼き込む」素朴な設計ならL=2程度が物理的な上限**、
**「都度UARTで送り直す」現在の設計ならLに上限はないが線形に遅くなる**、という二段構えの
制約になっている。CRT復元で扱えるモジュラスQ_total(=各qiの積)を大きくしたい場合は、
まず1チャネルあたり60ビットではなく、より大きい素数を探索する(`ntt_params.py`の
`--bits`を増やす)方が、チャネル数を増やすより現実的な選択肢になる。

## ソースからフルビルドする場合

[poly_mul_ntt_openfhe_n8192_soc/README.md](../poly_mul_ntt_openfhe_n8192_soc/README.md) と同じ手順
(HLS合成→Vivadoビルド→書き込み→ `vitis.bat -s` でのMicroBlazeソフトウェア作成→実行)。
HLSトップは `high_level/src/poly_mul_ntt.cpp`、Vivadoプロジェクト名は `crt8192`。
