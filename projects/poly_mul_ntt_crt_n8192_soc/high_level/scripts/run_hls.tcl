# run_hls.tcl
# Vitis HLS 2025.2 — poly_mul_ntt_crt (CRT/RNS分解 + Shoup乗算バタフライ版 負巡回NTT,
# N=8192, Lチャネル) バッチ合成
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\vitis-run.bat --mode hls --tcl <REPO_ROOT>\projects\poly_mul_ntt_crt_n8192_soc\high_level\scripts\run_hls.tcl --work_dir <REPO_ROOT>\projects\poly_mul_ntt_crt_n8192_soc\high_level"
#
# HLS プロジェクト出力: high_level/poly_mul_ntt_crt/solution1/impl/verilog/poly_mul_ntt_crt.v
#
# poly_mul_ntt_openfhe_n8192_soc との違い: トップ関数が m_axi ポインタに加えて
# チャネル選択スカラー ch (poly_mul_ntt_crt(coef_t *mem, int ch)) を取る。
# Block Design側では s_axi_CTRL (AXI-Lite, mem/chレジスタ含む) に加えて
# m_axi_gmem (AXI4 master) を共有BRAMコントローラに接続する必要がある。

set script_dir [file normalize [file dirname [info script]]]
set proj_root  [file normalize "$script_dir/.."]

cd $proj_root

open_project -reset poly_mul_ntt_crt
set_top poly_mul_ntt_crt
add_files src/poly_mul_ntt.cpp
open_solution solution1 -flow_target vivado
set_part xc7a100tcsg324-1
create_clock -period 10 -name default
csynth_design
export_design -format ip_catalog
exit
