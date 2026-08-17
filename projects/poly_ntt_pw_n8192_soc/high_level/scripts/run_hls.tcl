# run_hls.tcl
# Vitis HLS 2025.2 — poly_ntt_pw (poly_mul_ntt_crt_n8192_soc 3フェーズ分割版, フェーズ2
# pointwise乗算) バッチ合成。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_ntt_pw_n8192_soc\high_level\scripts\run_hls.tcl --work_dir <REPO_ROOT>\projects\poly_ntt_pw_n8192_soc\high_level"
#
# HLS プロジェクト出力: high_level/poly_ntt_pw/solution1/impl/verilog/poly_ntt_pw.v
#
# poly_mul_ntt_crt_n8192_soc/poly_mul_ntt.cpp との違い: NTTバタフライ・回転因子テーブルを
# 一切持たず、q/muのみのs_axilite CTRLレジスタで要素ごとのBarrett乗算だけを行う。

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_ntt_pw
set_top poly_ntt_pw
add_files src/poly_ntt_pw.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csynth_design
export_design -format ip_catalog
exit
