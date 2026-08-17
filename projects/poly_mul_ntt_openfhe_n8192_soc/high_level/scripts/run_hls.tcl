# run_hls.tcl
# Vitis HLS 2025.2 — poly_mul_ntt_n8192 (OpenFHE 方式 負巡回 NTT, N=8192) バッチ合成
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_mul_ntt_openfhe_n8192_soc\high_level\scripts\run_hls.tcl --work_dir <REPO_ROOT>\projects\poly_mul_ntt_openfhe_n8192_soc\high_level"
#
# HLS プロジェクト出力: high_level/poly_mul_ntt_n8192/solution1/impl/verilog/poly_mul_ntt_n8192.v
#
# poly_mul_ntt_openfhe_soc (N=4) と異なりトップ関数は m_axi ポインタ1本を取る
# (poly_mul_ntt_n8192(coef_t *mem)) ため、Block Design側では s_axi_control (AXI-Lite)
# に加えて m_axi_gmem (AXI4 master) を共有BRAMコントローラに接続する必要がある。

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_mul_ntt_n8192
set_top poly_mul_ntt_n8192
add_files src/poly_mul_ntt.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csynth_design
export_design -format ip_catalog
exit
