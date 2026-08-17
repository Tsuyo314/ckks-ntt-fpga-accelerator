# poly_ntt_inv_n8192_soc

[poly_ntt_fwd_n8192_soc](../poly_ntt_fwd_n8192_soc/) → [poly_ntt_pw_n8192_soc](../poly_ntt_pw_n8192_soc/)
→ **poly_ntt_inv_n8192_soc (Phase 3: 逆変換)** の3分割パイプラインの最終段。
Gentleman-Sande型の逆NTTで通常領域へ戻し、チャネル分の結果をCRT復元、
テストベクトルの期待値と突き合わせて `[OK]`/`[MISMATCH]` を表示する
(3プロジェクト中もっともタイミング余裕(WNS)が大きい)。

3プロジェクトの全体構成・実行順は [poly_ntt_fwd_n8192_soc/README.md](../poly_ntt_fwd_n8192_soc/README.md) を参照。
開発時の問題と対処(共有BRAMのアドレス空間が2の冪でない場合の対処など)は
[TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## 単体での実行

```
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"
python host/host_inv.py COM4
```

`../poly_ntt_pw_n8192_soc/host/host_pw.py` が出力した
`../../lib/generated/pipeline_n8192/pw_out.json` を入力(`--in`)、
`../../lib/generated/testvectors_n8192_crt2_q60.json` を期待値(`--vectors`)として、
`final_result.json` に最終結果を書き出す。

## ソースからフルビルドする場合

`poly_mul_ntt_openfhe_n8192_soc/README.md` と同じ手順。HLSトップは `high_level/src/poly_ntt_inv.cpp`、
Vivadoプロジェクト名は `inv8192`。
