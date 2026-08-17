# poly_mul_ntt_openfhe_n8192_soc

N=8192 の多項式(係数ごとに60ビットNTT素数 mod Q)をNTT(数論変換)ベースで乗算するFPGAアクセラレータ。
MicroBlaze SoC + Vitis HLSアクセラレータ + USB-UARTでPCと通信し、係数列を送ると乗算結果を返す。

処理内容やブロック図など詳しい設計は [FLOW.md](FLOW.md) / [NTT_ALGORITHM.md](NTT_ALGORITHM.md) を参照。
開発中に遭遇した問題と対処は [TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## クイックスタート(ビルド済みビットストリーム+ファームウェアを使う場合)

リビルド不要で試すだけなら、同梱の `low_level/bitstream/design_1_wrapper.bit`(FPGA側)と
`software/elf/poly_mul_openfhe_n8192_app.elf`(MicroBlaze側ファームウェア)を書き込むだけでよい。
**ビットストリームの書き込みだけではMicroBlazeにプログラムが載っていないため応答が返らない。
ELFのダウンロードも必須**(実機検証済み)。

```
# 1. ボードをUSB接続してビットストリームを書き込み (Windows/cmd.exe)
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"

# 2. MicroBlazeにファームウェア(ELF)をダウンロードして実行
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl software\elf\poly_mul_openfhe_n8192_app.elf"

# 3. PCから多項式係数を送って乗算結果を受け取る (要 pyserial)
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

# 4. MicroBlaze用ソフトウェア(プラットフォーム/アプリ)を作成
#    (create_platform.py/create_app.py は `vitis` Python APIを使うため、通常の python ではなく
#     vitis.bat -s 経由で実行する。同じセッションで先に chcp 65001 が必要 — 文字コードエラー対策)
cmd.exe /c "chcp 65001 && C:\AMDDesignTools\2025.2\Vitis\bin\vitis.bat -s software\scripts\create_platform.py"
cmd.exe /c "chcp 65001 && C:\AMDDesignTools\2025.2\Vitis\bin\vitis.bat -s software\scripts\create_app.py"
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl"

# 5. ホストスクリプトで送受信テスト
python host/host_bulk.py COM4 --count 3
```

Windows の MAX_PATH(260バイト)制限で `create_project.tcl` の IP登録に失敗する場合は、
`low_level` を `subst` で短いドライブレターにマッピングしてから実行するか、
スクリプト内の `ip_repo` を自環境の絶対パスに直接書き換える(詳細はスクリプト内コメント参照)。
`create_platform.py`/`create_app.py` も同様の理由でプロジェクトルートを
`subst W: <このプロジェクトへの絶対パス>` してから `W:\software\scripts\...` として実行する
(スクリプト内 `PROJ_ROOT = "W:"` 参照)。

`create_app.py` が `Cannot set workspace ... already in use` で失敗する場合、
直前の `create_platform.py`/`create_app.py` 実行が残した Vitis Server (`RigelApp`,
`java.exe` プロセス) がワークスペースをロックしたままになっている。該当プロセスを終了し、
`software/workspace/_ide/.wsdata/.lock` を削除してから再実行する。
