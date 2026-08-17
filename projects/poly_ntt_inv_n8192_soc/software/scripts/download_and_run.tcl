# download_and_run.tcl
# xsct (Vitis 2025.2) 経由で MicroBlaze に poly_ntt_inv_app.elf をダウンロードして実行する。
# 実行前に low_level/scripts/program_device.tcl でビットストリームを書き込んでおくこと。
#
# バッチ実行 (WSL):
#   cmd.exe /c "C:\AMDDesignTools\2025.2\Vitis\bin\xsct.bat <REPO_ROOT>\projects\poly_ntt_inv_n8192_soc\software\scripts\download_and_run.tcl"

set script_dir [file normalize [file dirname [info script]]]

if {[llength $argv] > 0} {
    set elf_file [file normalize [lindex $argv 0]]
} else {
    set elf_file [file normalize "$script_dir/../workspace/poly_ntt_inv_app/build/poly_ntt_inv_app.elf"]
}

if {![file exists $elf_file]} {
    error "ELF not found: $elf_file\nRun create_app.py first."
}
puts "INFO: ELF -> $elf_file"

connect
targets -set -filter {name =~ "MicroBlaze #*"}
rst -processor
dow $elf_file
con

puts "INFO: Download and run complete. Monitor UART output separately (host/*.py)."
