# program_device.tcl
# Vivado 2025.2 向け — 生成済みビットストリームを Nexys A7-100T に書き込む。
# 実行前にボードを USB 接続しておくこと。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source <REPO_ROOT>\projects\poly_ntt_inv_n8192_soc\low_level\scripts\program_device.tcl"

set script_dir [file normalize [file dirname [info script]]]

if {[llength $argv] > 0} {
    set bit_file [file normalize [lindex $argv 0]]
} else {
    set bit_file [file normalize "$script_dir/../inv8192.runs/impl_1/design_1_wrapper.bit"]
}

if {![file exists $bit_file]} {
    error "Bitstream not found: $bit_file\nRun create_project.tcl first."
}
puts "INFO: Bitstream -> $bit_file"

open_hw_manager
connect_hw_server -url localhost:3121
open_hw_target

set devices [get_hw_devices]
if {[llength $devices] == 0} {
    error "No JTAG device found. Check USB connection and driver."
}
set device [lindex $devices 0]
puts "INFO: Device -> $device"

current_hw_device $device
refresh_hw_device $device
set_property PROGRAM.FILE $bit_file $device
program_hw_devices $device

puts "INFO: Programming complete."
close_hw_manager
