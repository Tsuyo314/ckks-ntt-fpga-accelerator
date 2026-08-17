# Clock (100 MHz)
set_property PACKAGE_PIN E3 [get_ports clk]
set_property IOSTANDARD LVCMOS33 [get_ports clk]
create_clock -period 10.000 -name sys_clk [get_ports clk]

# Reset (btnC)
set_property PACKAGE_PIN N17 [get_ports rst]
set_property IOSTANDARD LVCMOS33 [get_ports rst]

# UART RX (PC -> FPGA)
set_property PACKAGE_PIN C4 [get_ports uart_rx]
set_property IOSTANDARD LVCMOS33 [get_ports uart_rx]

# UART TX (FPGA -> PC)
set_property PACKAGE_PIN D4 [get_ports uart_tx]
set_property IOSTANDARD LVCMOS33 [get_ports uart_tx]
