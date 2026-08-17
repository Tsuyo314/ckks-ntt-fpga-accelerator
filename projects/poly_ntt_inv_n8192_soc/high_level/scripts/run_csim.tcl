# run_csim.tcl
# Vitis HLS 2025.2 — poly_ntt_inv の C シミュレーション (csim_design)。
# Vivado/JTAG不要。tb_poly_ntt_inv.cpp が tb_data_inv.h (fpga/lib/ntt_reference.py で
# 計算したゴールデンデータ, HLS実装とは独立した経路) とのround-trip一致を検証する。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_ntt_inv_n8192_soc\high_level\scripts\run_csim.tcl --work_dir <REPO_ROOT>\projects\poly_ntt_inv_n8192_soc\high_level"
#
# 期待される標準出力: "TEST PASSED: 8192 elements OK"

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_ntt_inv_csim
set_top poly_ntt_inv
add_files src/poly_ntt_inv.cpp
add_files -tb src/tb_poly_ntt_inv.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csim_design
exit
