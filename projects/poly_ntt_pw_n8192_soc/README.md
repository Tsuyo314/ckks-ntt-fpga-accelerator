# poly_ntt_pw_n8192_soc

[poly_ntt_fwd_n8192_soc](../poly_ntt_fwd_n8192_soc/) → **poly_ntt_pw_n8192_soc (Phase 2: 点ごとの乗算)**
→ [poly_ntt_inv_n8192_soc](../poly_ntt_inv_n8192_soc/) の3分割パイプラインの2段目。
NTT領域の Â・B̂ をBarrett還元で点ごとに乗算する(回転因子テーブル不要のため3プロジェクト中
最もリソース使用量が小さい)。

3プロジェクトの全体構成・実行順は [poly_ntt_fwd_n8192_soc/README.md](../poly_ntt_fwd_n8192_soc/README.md) を参照。
開発時の問題と対処は [TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## 単体での実行

ビットストリームの書き込みだけではMicroBlazeにプログラムが載っていないため応答が返らない。
ELFのダウンロードも必須(実機検証済み)。

```
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl software\elf\poly_ntt_pw_app.elf"
python host/host_pw.py COM4
```

`../poly_ntt_fwd_n8192_soc/host/host_fwd.py` が出力した
`../../lib/generated/pipeline_n8192/fwd_out.json` を入力(`--in`)とし、
`pw_out.json` を出力する。

## ソースからフルビルドする場合

[poly_mul_ntt_openfhe_n8192_soc/README.md](../poly_mul_ntt_openfhe_n8192_soc/README.md) と同じ手順。
HLSトップは `high_level/src/poly_ntt_pw.cpp`、Vivadoプロジェクト名は `pw8192`。
