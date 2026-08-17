# run_csim.tcl
# Vitis HLS 2025.2 — poly_ntt_fwd の C シミュレーション (csim_design)。
# Vivado/JTAG不要。tb_poly_ntt_fwd.cpp が tb_data_fwd.h (fpga/lib/ntt_reference.py の
# ntt_forward_ct で計算したゴールデンデータ, HLS実装とは独立した経路) と突き合わせる。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_ntt_fwd_n8192_soc\high_level\scripts\run_csim.tcl --work_dir <REPO_ROOT>\projects\poly_ntt_fwd_n8192_soc\high_level"
#
# 期待される標準出力: "TEST PASSED: 16384 elements OK (A_HAT + B_HAT)"

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_ntt_fwd_csim
set_top poly_ntt_fwd
add_files src/poly_ntt_fwd.cpp
add_files -tb src/tb_poly_ntt_fwd.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csim_design
exit
