# run_csim.tcl
# Vitis HLS 2025.2 — poly_ntt_pw の C シミュレーション (csim_design)。
# Vivado/JTAG不要、数秒で終わる。tb_poly_ntt_pw.cpp が独立経路 (unsigned __int128の
# ネイティブmod演算) で期待値を計算し、HLS側のBarrett還元実装と突き合わせる。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_ntt_pw_n8192_soc\high_level\scripts\run_csim.tcl --work_dir <REPO_ROOT>\projects\poly_ntt_pw_n8192_soc\high_level"
#
# 期待される標準出力: "TEST PASSED: 8192 elements OK"

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_ntt_pw_csim
set_top poly_ntt_pw
add_files src/poly_ntt_pw.cpp
add_files -tb src/tb_poly_ntt_pw.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csim_design
exit
