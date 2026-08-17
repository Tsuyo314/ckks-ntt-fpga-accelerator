# poly_ntt_fwd_n8192_soc

[poly_mul_ntt_crt_n8192_soc](../poly_mul_ntt_crt_n8192_soc/) と同じCRT/RNS・Shoup乗算構成のNTT多項式乗算を、
タイミング制約(1つのビットストリームに順変換+点乗算+逆変換全部を詰め込むとBRAM使用率98.5%/WNS未達に
なる問題)を解消するため **3つの独立したビットストリームに分割**したうちの **Phase 1: 順変換(forward NTT)** 担当。

3プロジェクトは以下の順で連携して使う(それぞれ別ビットストリームなので、書き込み→実行→
次のプロジェクトへ書き込み直し、という順序になる):

```
[このプロジェクト] poly_ntt_fwd_n8192_soc  (順変換 A,B → Â,B̂)
        ↓ fwd_out.json
poly_ntt_pw_n8192_soc   (点ごとの乗算 Â・B̂ → Ĉ)
        ↓ pw_out.json
poly_ntt_inv_n8192_soc  (逆変換 Ĉ → C, CRT復元, 期待値と比較)
        ↓ final_result.json
```

中間結果は `../../lib/generated/pipeline_n8192/` 配下のJSONファイルでプロジェクト間を受け渡す
(`host_*.py` が自動生成)。開発時の問題と対処は [TROUBLESHOOTING.md](TROUBLESHOOTING.md) を参照。

同梱のパラメータはCRTチャネル数(60ビット素数の個数)**L=2**。この基板でのチャネル数の
実用上の限度と理由(BRAM使用率98.5%、増やすほど処理時間が線形に増える)は
[poly_mul_ntt_crt_n8192_soc/README.md の「チャネル数(L, 素数の個数)について」](../poly_mul_ntt_crt_n8192_soc/README.md#チャネル数l-素数の個数について)
を参照。

## 必要なもの

- Digilent Nexys A7-100T (xc7a100tcsg324-1)
- AMD Vivado / Vitis HLS 2025.2
- Python 3.8+ (`pip install -r ../../requirements.txt`)

## クイックスタート(3プロジェクト通しで実行)

各フェーズとも、ビットストリームの書き込みだけではMicroBlazeにプログラムが載っていないため
応答が返らない。**ELFのダウンロードも必須**(実機検証済み: 3フェーズ通しで最終結果 `[OK]` を確認済み)。

```
# --- Phase 1: 順変換 ---
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source low_level\scripts\program_device.tcl -tclargs low_level\bitstream\design_1_wrapper.bit"
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat software\scripts\download_and_run.tcl software\elf\poly_ntt_fwd_app.elf"
python host/host_fwd.py COM4 --count 3

# --- Phase 2: 点ごとの乗算 (../poly_ntt_pw_n8192_soc/ で実行) ---
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source ..\poly_ntt_pw_n8192_soc\low_level\scripts\program_device.tcl -tclargs ..\poly_ntt_pw_n8192_soc\low_level\bitstream\design_1_wrapper.bit"
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat ..\poly_ntt_pw_n8192_soc\software\scripts\download_and_run.tcl ..\poly_ntt_pw_n8192_soc\software\elf\poly_ntt_pw_app.elf"
python ../poly_ntt_pw_n8192_soc/host/host_pw.py COM4

# --- Phase 3: 逆変換 + CRT復元 + 期待値検証 (../poly_ntt_inv_n8192_soc/ で実行) ---
cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source ..\poly_ntt_inv_n8192_soc\low_level\scripts\program_device.tcl -tclargs ..\poly_ntt_inv_n8192_soc\low_level\bitstream\design_1_wrapper.bit"
cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat ..\poly_ntt_inv_n8192_soc\software\scripts\download_and_run.tcl ..\poly_ntt_inv_n8192_soc\software\elf\poly_ntt_inv_app.elf"
python ../poly_ntt_inv_n8192_soc/host/host_inv.py COM4
```

`host_inv.py` が `--vectors` の期待値(`../../lib/generated/testvectors_n8192_crt2_q60.json`)
と最終結果を突き合わせて `[OK]`/`[MISMATCH]` を表示する。

## ソースからフルビルドする場合

[poly_mul_ntt_openfhe_n8192_soc/README.md](../poly_mul_ntt_openfhe_n8192_soc/README.md) と同じ手順
(HLS合成→Vivadoビルド→書き込み→ `vitis.bat -s` でのMicroBlazeソフトウェア作成→実行)。
このプロジェクトのHLSトップは `high_level/src/poly_ntt_fwd.cpp`、Vivadoプロジェクト名は `fwd8192`。
`poly_ntt_pw_n8192_soc` / `poly_ntt_inv_n8192_soc` も同様にそれぞれのディレクトリで実施する。
