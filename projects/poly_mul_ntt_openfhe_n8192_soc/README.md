# poly_mul_ntt_openfhe_n8192_soc

N=8192 の多項式(係数ごとに60ビットNTT素数 mod Q)をNTT(数論変換)ベースで乗算するFPGAアクセラレータ。
MicroBlaze SoC + Vitis HLSアクセラレータ + USB-UARTでPCと通信し、係数列を送ると乗算結果を返す。

処理内容やブロック図など詳しい設計は [FLOW.md](FLOW.md) / [NTT_ALGORITHM.md](NTT_ALGORITHM.md) を参照。
開発中に遭遇した問題と対処は [TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## クイックスタート(ビルド済みビットストリームを使う場合)

リビルド不要で試すだけなら、同梱の `low_level/bitstream/design_1_wrapper.bit` を書き込むだけでよい。

```
# 1. ボードをUSB接続してJTAG経由で書き込み (Windows/cmd.exe)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"

# 2. PCから多項式係数を送って乗算結果を受け取る (要 pyserial)
python host/host_bulk.py COM4 --count 3
```

`host_bulk.py` は共有テストベクトル(`../../lib/generated/testvectors_n8192_q60.json`)を読み込み、
リファレンス実装(`../../lib/ntt_reference.py`)による期待値と突き合わせて `[OK]`/`[MISMATCH]` を表示する。

## ソースからフルビルドする場合

```
# 1. HLSでNTTアクセラレータを合成 (IPコアを high_level/ 配下に生成)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl high_level\scripts\run_hls.tcl --work_dir high_level"

# 2. Vivadoでブロックデザイン構築〜ビットストリーム生成
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\create_project.tcl"

# 3. 書き込み (ビルド直後は引数省略で低レベルの .runs 配下のビットストリームを使う)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl"

# 4. MicroBlaze用ソフトウェア(プラットフォーム/アプリ)を作成・書き込み
python software\scripts\create_platform.py
python software\scripts\create_app.py
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl"

# 5. ホストスクリプトで送受信テスト
python host/host_bulk.py COM4 --count 3
```

Windows の MAX_PATH(260バイト)制限で `create_project.tcl` の IP登録に失敗する場合は、
`low_level` を `subst` で短いドライブレターにマッピングしてから実行するか、
スクリプト内の `ip_repo` を自環境の絶対パスに直接書き換える(詳細はスクリプト内コメント参照)。
