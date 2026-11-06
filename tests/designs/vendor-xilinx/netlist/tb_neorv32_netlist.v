// SPDX-License-Identifier: GPL-3.0-or-later
//
// NEORV32's functional-simulation netlist in Verilog, as make.sh makes it, run from the program in
// its memory for 160 us, the window the design regression runs NEORV32's own testbench for: the
// same clock and reset, its UARTs looped back to each other as there, and every other input at the
// level NEORV32 gives it when it is open. Each instruction a core retires is printed as the raw
// trace its port shows, which a second simulator must print alike. It passes when both cores
// retired some.
//
// Icarus Verilog runs NEORV32's own testbench only in VHDL, and its Verilog one only around VHDL
// models, so the netlist has this one for Icarus to be held to.

`timescale 1ns / 1ps
`include "neorv32_netlist.vh"

module tb_neorv32_netlist;
    localparam integer T_CPU = 10;

    reg clk_en = 1'b0;
    reg rst_gen = 1'b0;
    reg clk_gen = 1'b0;
    initial #(30 * T_CPU / 2) rst_gen = 1'b1;
    initial #(60 * T_CPU / 2) clk_en = 1'b1;
    always #(T_CPU / 2) clk_gen = !clk_gen && clk_en;

    wire [`TRACE_WIDTH-1:0] trace0, trace1;
    wire uart0_txd, uart0_rtsn, uart1_txd, uart1_rtsn;

    neorv32_netlist u_netlist (
        .clk_i(clk_gen), .rstn_i(rst_gen), .rstn_ocd_o(), .rstn_wdt_o(),
        .trace_cpu0_o(trace0), .trace_cpu1_o(trace1),
        .jtag_tck_i(1'b0), .jtag_tdi_i(1'b0), .jtag_tdo_o(), .jtag_tms_i(1'b0),
        .smc_ioen_o(), .smc_sck_o(), .smc_csn_o(), .smc_sdo_o(), .smc_sdi_i(1'b0),
        .xbus_adr_o(), .xbus_dat_o(), .xbus_cti_o(), .xbus_tag_o(), .xbus_we_o(), .xbus_sel_o(),
        .xbus_stb_o(), .xbus_cyc_o(), .xbus_dat_i(32'd0), .xbus_ack_i(1'b0), .xbus_err_i(1'b0),
        .slink_rx_dat_i(32'd0), .slink_rx_src_i(4'd0), .slink_rx_val_i(1'b0),
        .slink_rx_lst_i(1'b0), .slink_rx_rdy_o(), .slink_tx_dat_o(), .slink_tx_dst_o(),
        .slink_tx_val_o(), .slink_tx_lst_o(), .slink_tx_rdy_i(1'b0),
        .gpio_dir_o(), .gpio_o(), .gpio_i(32'd0),
        .uart0_txd_o(uart0_txd), .uart0_rxd_i(uart1_txd), .uart0_rtsn_o(uart0_rtsn),
        .uart0_ctsn_i(uart1_rtsn),
        .uart1_txd_o(uart1_txd), .uart1_rxd_i(uart0_txd), .uart1_rtsn_o(uart1_rtsn),
        .uart1_ctsn_i(uart0_rtsn),
        .spi_clk_o(), .spi_dat_o(), .spi_dat_i(1'b0), .spi_csn_o(),
        .sdi_clk_i(1'b0), .sdi_dat_o(), .sdi_dat_i(1'b0), .sdi_csn_i(1'b1),
        .twi_sda_i(1'b1), .twi_sda_o(), .twi_scl_i(1'b1), .twi_scl_o(),
        .twd_sda_i(1'b1), .twd_sda_o(), .twd_scl_i(1'b1),
        .onewire_i(1'b1), .onewire_o(),
        .pwm_o(), .cfs_in_i(256'd0), .cfs_out_o(), .neoled_o(), .mtime_time_o(),
        .irq_msi_i(1'b0), .irq_mti_i(1'b0), .irq_mei_i(1'b0)
    );

    // The trace record's first field, `valid`, is its highest bit.
    integer retired0 = 0, retired1 = 0;
    always @(posedge clk_gen) begin
        if (trace0[`TRACE_WIDTH-1] === 1'b1) begin
            $display("cpu0 %0d %h", $time, trace0);
            retired0 = retired0 + 1;
        end
        if (trace1[`TRACE_WIDTH-1] === 1'b1) begin
            $display("cpu1 %0d %h", $time, trace1);
            retired1 = retired1 + 1;
        end
    end

    initial begin
        #160000;
        $display("cpu0 retired %0d, cpu1 %0d", retired0, retired1);
        if (retired0 > 0 && retired1 > 0) $display("PASS tb_neorv32_netlist");
        else $display("FAIL: a core retired nothing");
        $finish;
    end
endmodule
