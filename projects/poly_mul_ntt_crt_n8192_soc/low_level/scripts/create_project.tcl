# create_project.tcl
# Vivado 2025.2 向け — poly_mul_ntt_crt_n8192_soc:
# MicroBlaze + AXI Uartlite + 共有AXI BRAMコントローラ + 自作HLS IP(poly_mul_ntt_crt, s_axi_CTRL + m_axi_gmem)
# の Block Design (IP Integrator) を Tcl のみで構築し、
# Synthesis -> Implementation -> Write Bitstream -> Hardware Platform(XSA) 書き出しまで一括実行する。
#
# poly_mul_ntt_openfhe_n8192_soc との構成上の違い:
#   - HLS IPのs_axi_CTRLレジスタに mem ベースアドレスに加え q/mu/ninv/ninvsh/phase の
#     スカラーが追加されている (チャネルごとのモジュラス・回転因子テーブルは合成時
#     ROMではなくホストが呼び出しごとにmem経由で実行時ロードする2フェーズ構成のため。
#     poly_mul_ntt.cpp冒頭コメント・TROUBLESHOOTING.md参照)。
#   - 共有axi_bram_ctrl_polyは256KB (32bit幅x65536語) に拡張されている
#     (mem[0,N)=A, mem[N,2N)=B, mem[2N,3N)=TAB, mem[3N,4N)=TABSH の4ブロック構成)。
#
# バッチ実行 (WSL, Windows MAX_PATH対策のsubstドライブ経由):
#   cmd.exe /c "subst V: <REPO_ROOT>\projects\poly_mul_ntt_crt_n8192_soc\low_level && C:\AMDDesignTools\2025.2\Vivado\bin\vivado.bat -mode batch -source V:\scripts\create_project.tcl"
#
# 注意: apply_bd_automation の -config 辞書キー名や自動生成される外部ポート名、
# また m_axi_gmem と MicroBlaze 側マスタを同一の axi_bram_ctrl スレーブへ束ねる際に
# Vivado が自動挿入する AXI Interconnect/SmartConnect の挙動は、実行してみないと
# 確定しない部分がある。エラーが出た場合はメッセージを見て該当箇所を調整し、
# catch{close_project}+ -force により何度でも再実行できる。

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   [file normalize "$script_dir/.."]
set proj_name  crt8192
set part       xc7a100tcsg324-1
set bd_name    design_1
# 注: ip_repo はスクリプトの場所からの相対パスで解決する。
# Windows MAX_PATH(260バイト)制限でIP登録に失敗する場合は、
# `subst V: <このプロジェクトの low_level への絶対パス>` で短いドライブレターに
# マッピングしてから `V:\scripts\create_project.tcl` を実行するか、
# 上記 ip_repo を自環境の絶対パスに直接書き換えてください。
set ip_repo    [file normalize "$proj_dir/../high_level/poly_mul_ntt_crt/solution1/impl/ip"]
catch { close_project }
create_project $proj_name $proj_dir -part $part -force

# ------------------------------------------------------------------
# 自作HLS IPのリポジトリ登録
# ------------------------------------------------------------------
set_property ip_repo_paths $ip_repo [current_fileset]
update_ip_catalog

# ------------------------------------------------------------------
# Block Design 作成
# ------------------------------------------------------------------
create_bd_design $bd_name

# ------------------------------------------------------------------
# MicroBlaze 追加 + Block Automation 相当
# (local_memは16KBに縮小: バルクデータは共有axi_bram_ctrl側に置くため。
#  共有axi_bram_ctrl_polyを256KBに拡張した結果チップ全体のBRAMがわずかに超過したため、
#  さらに切り詰めてBRAM予算に余裕を持たせている。TROUBLESHOOTING.md参照)
# ------------------------------------------------------------------
create_bd_cell -type ip -vlnv xilinx.com:ip:microblaze:11.0 microblaze_0

apply_bd_automation -rule xilinx.com:bd_rule:microblaze -config {
    local_mem {16KB}
    ecc {None}
    cache {None}
    debug_module {Debug Only}
    axi_periph {Enabled}
    axi_intc {0}
    clk {New External Port (ACLK)}
} [get_bd_cells microblaze_0]

# ------------------------------------------------------------------
# AXI Uartlite 追加 + Connection Automation
# ------------------------------------------------------------------
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_uartlite:2.0 axi_uartlite_0
set_property CONFIG.C_BAUDRATE 115200 [get_bd_cells axi_uartlite_0]

apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config {
    Master {/microblaze_0 (Periph)}
    Clk {Auto}
} [get_bd_intf_pins axi_uartlite_0/S_AXI]

# rx/tx を外部ポート化して固定名にリネーム
if {[llength [get_bd_pins -quiet axi_uartlite_0/rx]] > 0} {
    make_bd_pins_external  [get_bd_pins axi_uartlite_0/rx]
    make_bd_pins_external  [get_bd_pins axi_uartlite_0/tx]
    set_property name uart_rx [get_bd_ports rx_0]
    set_property name uart_tx [get_bd_ports tx_0]
} else {
    make_bd_intf_pins_external [get_bd_intf_pins axi_uartlite_0/UART]
    set_property name uart [get_bd_intf_ports UART_0]
}

# ------------------------------------------------------------------
# 共有 AXI BRAM コントローラ (poly係数バッファ + 回転因子テーブルスロット,
# MicroBlazeとHLSコア両方からアクセス)
# 32bit幅 x 65536語 = 256KB (= 4*N coef_t * 2 x 32bit語。レイアウトは
# mem[0,N)=A, mem[N,2N)=B, mem[2N,3N)=TAB, mem[3N,4N)=TABSH。
# TAB/TABSHは phase=0 で RFWD/RFWD_SH, phase=1 で RINV/RINV_SH として
# ホストが使い回す領域 — poly_mul_ntt.cpp 冒頭コメント参照)
# ------------------------------------------------------------------
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_poly
set_property CONFIG.SINGLE_PORT_BRAM {1} [get_bd_cells axi_bram_ctrl_poly]

create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 blk_mem_gen_poly
set_property CONFIG.Memory_Type {True_Dual_Port_RAM} [get_bd_cells blk_mem_gen_poly]
set_property CONFIG.Write_Width_A {32} [get_bd_cells blk_mem_gen_poly]
set_property CONFIG.Write_Depth_A {65536} [get_bd_cells blk_mem_gen_poly]
set_property CONFIG.Read_Width_A {32} [get_bd_cells blk_mem_gen_poly]
set_property CONFIG.Write_Width_B {32} [get_bd_cells blk_mem_gen_poly]
set_property CONFIG.Read_Width_B {32} [get_bd_cells blk_mem_gen_poly]

connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_poly/BRAM_PORTA] [get_bd_intf_pins blk_mem_gen_poly/BRAM_PORTA]

# MicroBlaze (UART充填/排出用) から axi_bram_ctrl_poly へ接続
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config {
    Master {/microblaze_0 (Periph)}
    Clk {Auto}
} [get_bd_intf_pins axi_bram_ctrl_poly/S_AXI]

# ------------------------------------------------------------------
# 自作HLS IP (poly_mul_ntt_crt, s_axi_CTRL + m_axi_gmem) 追加
# ------------------------------------------------------------------
create_bd_cell -type ip -vlnv xilinx.com:hls:poly_mul_ntt_crt:1.0 poly_mul_ntt_crt_0

# 制御レジスタ (mem ベースアドレス・ch含む) は MicroBlaze から AXI-Lite で叩く
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config {
    Master {/microblaze_0 (Periph)}
    Clk {Auto}
} [get_bd_intf_pins poly_mul_ntt_crt_0/s_axi_CTRL]

# m_axi_gmem (HLSコアのバルクメモリアクセス用マスタ) を共有BRAMコントローラへ接続。
# 既存の axi_bram_ctrl_poly/S_AXI に2本目のマスタとしてぶら下がるため、
# Vivado が SmartConnect/AXI Interconnect を自動挿入して調停する。
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config {
    Slave {/axi_bram_ctrl_poly/S_AXI}
    intc_ip {Auto}
    Clk {Auto}
} [get_bd_intf_pins poly_mul_ntt_crt_0/m_axi_gmem]

# ------------------------------------------------------------------
# クロック外部ポートを固定名にリネーム (wrapper生成前)
# ------------------------------------------------------------------
set clk_port [get_bd_ports -filter {DIR == "I" && TYPE == "clk"}]
if {[llength $clk_port] == 1} {
    set_property name clk $clk_port
    set_property CONFIG.FREQ_HZ 100000000 [get_bd_ports clk]
}

# ------------------------------------------------------------------
# 外部リセットポートの追加
# ------------------------------------------------------------------
create_bd_port -dir I -type rst rst
set_property CONFIG.POLARITY {ACTIVE_HIGH} [get_bd_ports rst]

set psr_cells [get_bd_cells -quiet -filter {VLNV =~ "*proc_sys_reset*"}]
foreach psr $psr_cells {
    connect_bd_net [get_bd_ports rst] [get_bd_pins $psr/ext_reset_in]
}

# ------------------------------------------------------------------
# アドレス割り当て (自動)
# ------------------------------------------------------------------
assign_bd_address

# axi_bram_ctrl の (読み取り専用の) MEM_DEPTH は、blk_mem_gen側の深度設定では
# なく、アドレスエディタで割り当てられたアドレスセグメントの RANGE から逆算
# される (poly_mul_ntt_openfhe_n8192_soc で発見・検証済み)。assign_bd_address の
# デフォルトは狭い範囲しか割り当てないため、256KB(0x40000)に明示的に広げ、
# 両マスタ(MicroBlaze / poly_mul_ntt_crt_0の m_axi_gmem)から見たセグメントを
# 一致させる。
foreach space [get_bd_addr_spaces] {
    foreach seg [get_bd_addr_segs -of $space] {
        if {[string match "*axi_bram_ctrl_poly*" $seg] && ![string match "/axi_bram_ctrl_poly/*" $seg]} {
            set off [get_property OFFSET $seg]
            assign_bd_address -offset $off -range 256K $seg
        }
    }
}
set_property CONFIG.Write_Depth_A {65536} [get_bd_cells blk_mem_gen_poly]

# ------------------------------------------------------------------
# Validate
# ------------------------------------------------------------------
validate_bd_design
puts "INFO: axi_bram_ctrl_poly MEM_DEPTH (words) = [get_property CONFIG.MEM_DEPTH [get_bd_cells axi_bram_ctrl_poly]]"

# ------------------------------------------------------------------
# Wrapper生成
# ------------------------------------------------------------------
generate_target all [get_files $proj_dir/$proj_name.srcs/sources_1/bd/$bd_name/$bd_name.bd]

make_wrapper -files [get_files $proj_dir/$proj_name.srcs/sources_1/bd/$bd_name/$bd_name.bd] -top
add_files -norecurse $proj_dir/$proj_name.srcs/sources_1/bd/$bd_name/hdl/${bd_name}_wrapper.v
set_property top ${bd_name}_wrapper [current_fileset]
update_compile_order -fileset sources_1

# ------------------------------------------------------------------
# 制約ファイル追加
# ------------------------------------------------------------------
add_files -fileset [get_filesets constrs_1] $proj_dir/constraints/nexys_a7.xdc

# ------------------------------------------------------------------
# 合成・実装・ビットストリーム生成
# ------------------------------------------------------------------
puts "INFO: Starting synthesis..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    error "Synthesis failed. Check $proj_name.runs/synth_1/runme.log"
}
puts "INFO: Synthesis complete."

# フェイルファスト・チェックポイント: 実装(数十分かかる)に進む前に、Block Design
# 全体でのBlock RAM使用率を確認する。トップレベルの ${bd_name}_wrapper_utilization_synth.rpt
# は各IPがout-of-context合成のブラックボックスとして扱われるため使用量が0と出てしまう
# (実際に確認済み)。各サブIPの synth_1 run (design_1_*_synth_1) が個別に持つ
# utilization_synth.rpt を合算することで、実装前におおよその全体像を得る。
set total_bram 0.0
set bram_reports [glob -nocomplain "$proj_dir/${proj_name}.runs/design_1_*_synth_1/*_utilization_synth.rpt"]
foreach rpt $bram_reports {
    set fh [open $rpt r]
    set txt [read $fh]
    close $fh
    if {[regexp {Block RAM Tile\s*\|\s*([0-9.]+)\s*\|} $txt -> used]} {
        set total_bram [expr {$total_bram + $used}]
    }
}
puts "INFO: サブIP合計 Block RAM Tile 使用量 (概算, 実装前) = $total_bram / 270"
if {$total_bram > 270} {
    error "推定Block RAM使用量($total_bram)が容量(270)を超えています。実装前に中断します。各design_1_*_synth_1/*_utilization_synth.rptを確認してください。"
}

puts "INFO: Starting implementation and bitstream generation..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    error "Implementation failed. Check $proj_name.runs/impl_1/runme.log"
}
puts "INFO: Implementation complete."

set bit_file "$proj_dir/${proj_name}.runs/impl_1/${bd_name}_wrapper.bit"
puts "INFO: Done. Bitstream -> $bit_file"

# ------------------------------------------------------------------
# ハードウェアプラットフォーム(XSA)書き出し (Vitisソフトウェア側の入力)
# ------------------------------------------------------------------
open_run impl_1
set xsa_file "$proj_dir/${proj_name}.runs/impl_1/${bd_name}_wrapper.xsa"
write_hw_platform -fixed -include_bit -force $xsa_file
puts "INFO: Hardware platform -> $xsa_file"

puts ""
puts "Next: program the device with program_device.tcl"
puts "  vivado.bat -mode batch -source <REPO_ROOT>\projects\poly_mul_ntt_crt_n8192_soc\\low_level\\scripts\\program_device.tcl"
puts "  (note: proj_name is now '$proj_name', .runs dir is '$proj_name.runs')"
