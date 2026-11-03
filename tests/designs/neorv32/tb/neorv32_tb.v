// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright (c) NEORV32 contributors.
// Copyright (c) 2020-2026, Stephan Nolting. All rights reserved.
// Copyright (c) 2026 the Enzan authors, for the translation into Verilog.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// NEORV32's sim/neorv32_tb.vhd in Verilog, around the processor's VHDL and its VHDL simulation
// models: the same configuration, clock, reset, buses and models, and the same on-chip debugger
// sequence over JTAG, its reports displayed, so that its reports and logs are those of the VHDL
// testbench under GHDL (run.sh). No other free simulator instantiates VHDL from Verilog, and that
// run is what checks this testbench.
//
// What differs is what a Verilog net is: a weak pull, VHDL's 'H' or 'L', is a pulled net here,
// tri1 or tri0, which a design reads as a strong 1 or 0.
`timescale 1ns / 1ps

module neorv32_tb;

  // The testbench's generics' defaults, which run.sh runs it with.
  localparam integer CLOCK_FREQUENCY = 100000000;
  localparam integer PSRAM_SIZE = 8 * 1024;
  localparam integer CACHE_BLOCK_SIZE = 32;
  localparam [31:0] EXT_MEM_A_BASE = 32'h00000000;
  localparam [31:0] EXT_MEM_B_BASE = 32'h80000000;
  localparam integer EXT_MEM_A_SIZE = 64;
  localparam integer EXT_MEM_B_SIZE = 64;

  // The CPU clock's period in nanoseconds, and a JTAG clock's.
  localparam integer T_CPU = 10;
  localparam integer T_JTAG = 80;

  // -- Clock and reset ----------------------------------------------------------------------

  reg clk_en = 1'b0;
  reg rst_gen = 1'b0;
  reg clk_gen;

  initial #(30 * T_CPU / 2) rst_gen = 1'b1;
  initial #(60 * T_CPU / 2) clk_en = 1'b1;
  always #(T_CPU / 2) clk_gen = !clk_gen && clk_en;

  // -- IO connections -----------------------------------------------------------------------

  wire uart0_txd, uart0_ctsn, uart1_txd, uart1_ctsn;
  wire [31:0] gpio_dir, gpio_out, gpio_in;
  wire twi_scl_o, twi_sda_o, twd_sda_o, onewire_o;
  wire [7:0] spi_csn;
  wire spi_do, spi_di, spi_clk, sdi_do;
  wire [1:0] smc_csn;
  wire smc_clk, smc_do, smc_di;
  reg jtag_tck, jtag_tms, jtag_tdi;
  wire jtag_tdo;
  reg msi = 1'b0;
  reg mti = 1'b0;
  reg mei = 1'b0;

  // GPIO with weak pull-downs; the two-wire and one-wire buses, which modules only pull low,
  // with weak pull-ups; and the serial memory's data, with weak pull-downs.
  tri0 [31:0] gpio;
  tri1 i2c_sda, i2c_scl, onewire;
  tri0 [3:0] psram_data;

  // Stream link, through the FIFO looping it back.
  wire [31:0] slink_tx_data, slink_rx_data;
  wire [3:0] slink_tx_addr, slink_rx_addr;
  wire slink_tx_valid, slink_tx_last, slink_tx_ready;
  wire slink_rx_valid, slink_rx_last, slink_rx_ready;

  // XBUS, its records as vectors, the first field highest: a request is the address [76:45], the
  // write data [44:13], the cycle type, the tag, write enable [6], byte enables [5:2], strobe [1]
  // and cycle [0]; a response the read data [33:2], acknowledge [1] and error [0].
  wire [31:0] xbus_adr, xbus_dat_o, xbus_dat_i;
  wire [2:0] xbus_cti, xbus_tag;
  wire [3:0] xbus_sel;
  wire xbus_we, xbus_stb, xbus_cyc, xbus_ack, xbus_err;
  wire [76:0] xbus_core_req = {xbus_adr, xbus_dat_o, xbus_cti, xbus_tag, xbus_we, xbus_sel,
                               xbus_stb, xbus_cyc};
  wire [33:0] xbus_core_rsp;
  assign {xbus_dat_i, xbus_ack, xbus_err} = xbus_core_rsp;
  wire [76:0] xbus_ext_mem_a_req, xbus_ext_mem_b_req, xbus_mmio_req, xbus_trig_req;
  wire [76:0] xbus_fmem_data_req, xbus_fmem_tag_req, xbus_rom_req, xbus_ram_req;
  wire [33:0] xbus_mmio_rsp, xbus_fmem_data_rsp, xbus_fmem_tag_rsp;
  reg [33:0] xbus_trig_rsp;
  reg [33:0] xbus_rom_rsp = 34'd0;
  reg [33:0] xbus_ram_rsp = 34'd0;

  // -- JTAG test ----------------------------------------------------------------------------

  task jtag_tck_cycle(input tms, input tdi, output tdo);
    begin
      jtag_tck = 1'b0;
      jtag_tms = tms;
      jtag_tdi = tdi;
      #(T_JTAG / 4);
      jtag_tck = 1'b1;
      tdo = jtag_tdo;
      #(T_JTAG / 2);
      jtag_tck = 1'b0;
      #(T_JTAG / 4);
    end
  endtask

  task jtag_reset;
    reg tdo;
    integer i;
    begin
      for (i = 0; i <= 8; i = i + 1) jtag_tck_cycle(1'b1, 1'b0, tdo); // to RESET from any state
      jtag_tck_cycle(1'b0, 1'b0, tdo); // RUN_IDLE
    end
  endtask

  task jtag_shift_ir(input [4:0] ir);
    reg tdo;
    integer i;
    begin
      jtag_tck_cycle(1'b1, 1'b0, tdo); // SELECT-DR-SCAN
      jtag_tck_cycle(1'b1, 1'b0, tdo); // SELECT-IR-SCAN
      jtag_tck_cycle(1'b0, 1'b0, tdo); // CAPTURE-IR
      jtag_tck_cycle(1'b0, 1'b0, tdo); // SHIFT-IR
      for (i = 0; i <= 3; i = i + 1) jtag_tck_cycle(1'b0, ir[i], tdo);
      jtag_tck_cycle(1'b1, ir[4], tdo); // last bit: EXIT1-IR
      jtag_tck_cycle(1'b1, 1'b0, tdo); // UPDATE-IR
      jtag_tck_cycle(1'b0, 1'b0, tdo); // RUN-TEST/IDLE
    end
  endtask

  task jtag_shift_dr(input [40:0] din, output [40:0] dout);
    reg tdo;
    integer i;
    begin
      jtag_tck_cycle(1'b1, 1'b0, tdo); // SELECT-DR-SCAN
      jtag_tck_cycle(1'b0, 1'b0, tdo); // CAPTURE-DR
      jtag_tck_cycle(1'b0, 1'b0, tdo); // SHIFT-DR
      for (i = 0; i <= 40; i = i + 1) begin
        jtag_tck_cycle(i == 40, din[i], tdo); // SHIFT-DR, and EXIT1-DR with the last
        dout[i] = tdo;
      end
      jtag_tck_cycle(1'b1, 1'b0, tdo); // UPDATE-DR
      jtag_tck_cycle(1'b0, 1'b0, tdo); // RUN-TEST/IDLE
    end
  endtask

  task dmi_write(input [6:0] addr, input [31:0] data);
    reg [40:0] dout;
    begin
      jtag_shift_ir(5'b10001); // IR = DMI
      jtag_shift_dr({addr, data, 2'b10}, dout); // op = write
    end
  endtask

  task dmi_read(input [6:0] addr, output [31:0] data);
    reg [40:0] dout;
    begin
      jtag_shift_ir(5'b10001); // IR = DMI
      jtag_shift_dr({addr, 32'h00000000, 2'b01}, dout); // op = read
      jtag_shift_dr(41'd0, dout); // op = NOP, reading back
      data = dout[33:2];
    end
  endtask

  // Its reports, as the VHDL testbench's are: a note as its message, an error after "error: ".
  reg [31:0] tmp_v;
  initial begin : jtag_test
    jtag_tck = 1'b0;
    jtag_tms = 1'b0;
    jtag_tdi = 1'b0;
    #(100 * T_CPU);

    // reset JTAG tap --
    #(10 * T_CPU);
    $display("[TB:JTAG] Resetting JTAG tap...");
    jtag_reset;

    // enable DM (dmcontrol[0]) --
    #(10 * T_CPU);
    $display("[TB:JTAG] Enabling debug module...");
    dmi_write(7'b0010000, 32'h00000001); // dmcontrol

    // authenticate (assuming the default authenticator) --
    #(10 * T_CPU);
    $display("[TB:JTAG] Authenticating...");
    dmi_write(7'b0110000, 32'h00000001); // authdata
    dmi_read(7'b0010001, tmp_v); // dmstatus
    if (tmp_v[7:0] == 8'h83) $display("[TB:JTAG] JTAG access authenticated.");
    else $display("error: [TB:JTAG] Cannot authenticate! (dmstatus = 0x%h)", tmp_v);

    // halt CPU core 0 --
    $display("[TB:JTAG] Halting CPU-0...");
    #(10 * T_CPU);
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    dmi_write(7'b0010000, tmp_v | 32'h80000000); // set haltreq (bit 31)
    dmi_read(7'b0010001, tmp_v); // dmstatus
    if (tmp_v[8] === 1'b1) $display("[TB:JTAG] CPU-0 halted.");
    else $display("error: [TB:JTAG] Cannot halt CPU-0! (dmstatus = 0x%h)", tmp_v);

    // write data word to testbench "external IO memory" --
    #(10 * T_CPU);
    $display("[TB:JTAG] Writing to memory (0xF0000000) via program buffer...");
    dmi_write(7'b0100000, 32'h00942023); // progbuf0 = sw x9, 0(x8)
    dmi_write(7'b0100001, 32'h00000013); // progbuf1 = nop
    dmi_write(7'b0000100, 32'hF0000000); // data0 = memory address
    dmi_write(7'b0010111, 32'h00231008); // command = write data0 -> x8
    dmi_write(7'b0000100, 32'h0cd7e571); // data0 = data ("ocd test!")
    dmi_write(7'b0010111, 32'h00271009); // command = write data0 -> x9 + postexec
    dmi_read(7'b0010110, tmp_v); // abstractcs
    if (tmp_v[10:8] === 3'b000) $display("[TB:JTAG] Memory write successful.");
    else $display("error: [TB:JTAG] Memory write FAILED! (abstractcs = 0x%h)", tmp_v);

    // reset CPU --
    #(10 * T_CPU);
    $display("[TB:JTAG] Resetting SoC and halting CPU-0...");
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    dmi_write(7'b0010000, tmp_v | 32'h00000002); // set ndmreset (bit 1)
    #(2 * T_CPU);
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    dmi_write(7'b0010000, (tmp_v & 32'hFFFFFFFD) | 32'h80000000); // clear ndmreset, set haltreq
    #(2 * T_CPU);
    dmi_read(7'b0010001, tmp_v); // dmstatus
    if (tmp_v[19] === 1'b1 && tmp_v[9] === 1'b1) $display("[TB:JTAG] CPU-0 reset and halted.");
    else $display("error: [TB:JTAG] CPU-0 not reset/halted! (dmstatus = 0x%h)", tmp_v);

    // resume CPU --
    #(10 * T_CPU);
    $display("[TB:JTAG] Resuming CPU-0...");
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    dmi_write(7'b0010000, (tmp_v & 32'h7FFFFFFF) | 32'h40000000); // clear haltreq, set resumereq
    dmi_read(7'b0010001, tmp_v); // dmstatus
    if (tmp_v[17] === 1'b1) $display("[TB:JTAG] CPU-0 resumed.");
    else $display("error: [TB:JTAG] Cannot resume CPU-0! (dmstatus = 0x%h)", tmp_v);
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    dmi_write(7'b0010000, tmp_v & 32'hBFFFFFFF); // clear resumereq (bit 30)

    // disable DM --
    #(10 * T_CPU);
    $display("[TB:JTAG] Disabling debug module...");
    dmi_write(7'b0010000, 32'h00000000); // dmcontrol
    dmi_read(7'b0010000, tmp_v); // dmcontrol
    if (tmp_v === 32'h00000000) $display("[TB:JTAG] Debug module disabled.");
    else $display("error: [TB:JTAG] Debug module not disabled! (dmstatus = 0x%h)", tmp_v);
  end

  // -- The processor ------------------------------------------------------------------------

  neorv32_top #(
    .CLOCK_FREQUENCY(CLOCK_FREQUENCY), .DUAL_CORE_EN(1'b1), .TRACE_PORT_EN(1'b0),
    .TRACE_SIMLOG_EN(1'b1), .BOOT_MODE_SELECT(2), .BOOT_ADDR_CUSTOM(32'h00000000),
    .OCD_EN(1'b1), .OCD_NUM_HW_TRIGGERS(3), .OCD_AUTHENTICATION(1'b1),
    .RISCV_ISA_C(1'b1), .RISCV_ISA_E(1'b0), .RISCV_ISA_M(1'b1), .RISCV_ISA_U(1'b1),
    .RISCV_ISA_Zaamo(1'b1), .RISCV_ISA_Zalrsc(1'b1), .RISCV_ISA_Zba(1'b1), .RISCV_ISA_Zbb(1'b1),
    .RISCV_ISA_Zbc(1'b1), .RISCV_ISA_Zbkb(1'b1), .RISCV_ISA_Zbkc(1'b1), .RISCV_ISA_Zbkx(1'b1),
    .RISCV_ISA_Zbs(1'b1), .RISCV_ISA_Zcb(1'b1), .RISCV_ISA_Zcmop(1'b1), .RISCV_ISA_Zfinx(1'b1),
    .RISCV_ISA_Zibi(1'b1), .RISCV_ISA_Zicntr(1'b1), .RISCV_ISA_Zicond(1'b1),
    .RISCV_ISA_Zihpm(1'b1), .RISCV_ISA_Zimop(1'b1), .RISCV_ISA_Zknd(1'b1), .RISCV_ISA_Zkne(1'b1),
    .RISCV_ISA_Zknh(1'b1), .RISCV_ISA_Zksed(1'b1), .RISCV_ISA_Zksh(1'b1), .RISCV_ISA_Zmmul(1'b1),
    .RISCV_ISA_Smcntrpmf(1'b1), .RISCV_ISA_Xcfu(1'b1),
    .CPU_CONSTT_BR_EN(1'b0), .CPU_FAST_MUL_EN(1'b1), .CPU_FAST_MUL_REGS(1),
    .CPU_FAST_SHIFT_EN(1'b1), .CPU_RF_ARCH_SEL(0),
    .PMP_NUM_REGIONS(5), .PMP_MIN_GRANULARITY(4), .PMP_TOR_MODE_EN(1'b1), .PMP_NAP_MODE_EN(1'b1),
    .HPM_NUM_CNTS(29), .HPM_CNT_WIDTH(64),
    .IMEM_EN(1'b1), .IMEM_BASE(32'h00000000), .IMEM_SIZE(32 * 1024), .IMEM_OUTREG_EN(1'b0),
    .DMEM_EN(1'b1), .DMEM_BASE(32'h80000000), .DMEM_SIZE(8 * 1024), .DMEM_OUTREG_EN(1'b1),
    .ICACHE_EN(1'b1), .ICACHE_NUM_BLOCKS(64), .DCACHE_EN(1'b1), .DCACHE_NUM_BLOCKS(32),
    .CACHE_BLOCK_SIZE(CACHE_BLOCK_SIZE), .CACHE_BURSTS_EN(1'b1), .CACHE_UC_BASE(32'hF0000000),
    .SMC_EN(1'b1), .SMC_BASE(32'hE0000000),
    .XBUS_EN(1'b1), .XBUS_TIMEOUT(2048), .XBUS_REGSTAGE_EN(1'b1),
    .IO_GPIO_NUM(32), .IO_GPIO_DIR_EN(1'b1), .IO_CLINT_EN(1'b1), .IO_UART0_EN(1'b1),
    .IO_UART0_RX_FIFO(32), .IO_UART0_TX_FIFO(32), .IO_UART1_EN(1'b1), .IO_UART1_RX_FIFO(1),
    .IO_UART1_TX_FIFO(1), .IO_SPI_EN(1'b1), .IO_SPI_FIFO(4), .IO_SDI_EN(1'b1), .IO_SDI_FIFO(4),
    .IO_TWI_EN(1'b1), .IO_TWI_FIFO(4), .IO_TWD_EN(1'b1), .IO_TWD_RX_FIFO(4), .IO_TWD_TX_FIFO(4),
    .IO_PWM_NUM(8), .IO_WDT_EN(1'b1), .IO_TRNG_EN(1'b1), .IO_TRNG_FIFO(4), .IO_TRNG_NUM_RO(3),
    .IO_TRNG_NUM_INV(5), .IO_TRNG_NUM_RBIT(64), .IO_CFS_EN(1'b1), .IO_NEOLED_EN(1'b1),
    .IO_NEOLED_TX_FIFO(8), .IO_GPTMR_NUM(4), .IO_ONEWIRE_EN(1'b1), .IO_ONEWIRE_FIFO(8),
    .IO_DMA_EN(1'b1), .IO_DMA_DSC_FIFO(8), .IO_SLINK_EN(1'b1), .IO_SLINK_RX_FIFO(4),
    .IO_SLINK_TX_FIFO(1), .IO_TRACEBUF_EN(1'b1), .IO_TRACEBUF_DEPTH(32)
  ) neorv32_top_inst (
    .clk_i(clk_gen), .rstn_i(rst_gen), .rstn_ocd_o(), .rstn_wdt_o(),
    .trace_cpu0_o(), .trace_cpu1_o(),
    .jtag_tck_i(jtag_tck), .jtag_tdi_i(jtag_tdi), .jtag_tdo_o(jtag_tdo), .jtag_tms_i(jtag_tms),
    .smc_ioen_o(), .smc_sck_o(smc_clk), .smc_csn_o(smc_csn), .smc_sdo_o(smc_do),
    .smc_sdi_i(smc_di),
    .xbus_adr_o(xbus_adr), .xbus_dat_o(xbus_dat_o), .xbus_cti_o(xbus_cti), .xbus_tag_o(xbus_tag),
    .xbus_we_o(xbus_we), .xbus_sel_o(xbus_sel), .xbus_stb_o(xbus_stb), .xbus_cyc_o(xbus_cyc),
    .xbus_dat_i(xbus_dat_i), .xbus_ack_i(xbus_ack), .xbus_err_i(xbus_err),
    .slink_rx_dat_i(slink_rx_data), .slink_rx_src_i(slink_rx_addr),
    .slink_rx_val_i(slink_rx_valid), .slink_rx_lst_i(slink_rx_last),
    .slink_rx_rdy_o(slink_rx_ready), .slink_tx_dat_o(slink_tx_data),
    .slink_tx_dst_o(slink_tx_addr), .slink_tx_val_o(slink_tx_valid),
    .slink_tx_lst_o(slink_tx_last), .slink_tx_rdy_i(slink_tx_ready),
    .gpio_dir_o(gpio_dir), .gpio_o(gpio_out), .gpio_i(gpio_in),
    .uart0_txd_o(uart0_txd), .uart0_rxd_i(uart1_txd), .uart0_rtsn_o(uart0_ctsn),
    .uart0_ctsn_i(uart1_ctsn),
    .uart1_txd_o(uart1_txd), .uart1_rxd_i(uart0_txd), .uart1_rtsn_o(uart1_ctsn),
    .uart1_ctsn_i(uart0_ctsn),
    .spi_clk_o(spi_clk), .spi_dat_o(spi_do), .spi_dat_i(spi_di), .spi_csn_o(spi_csn),
    .sdi_clk_i(spi_clk), .sdi_dat_o(sdi_do), .sdi_dat_i(spi_do), .sdi_csn_i(spi_csn[7]),
    .twi_sda_i(i2c_sda), .twi_sda_o(twi_sda_o), .twi_scl_i(i2c_scl), .twi_scl_o(twi_scl_o),
    .twd_sda_i(i2c_sda), .twd_sda_o(twd_sda_o), .twd_scl_i(i2c_scl),
    .onewire_i(onewire), .onewire_o(onewire_o),
    .pwm_o(), .cfs_in_i(256'd0), .cfs_out_o(), .neoled_o(), .mtime_time_o(),
    .irq_msi_i(msi), .irq_mti_i(mti), .irq_mei_i(mei)
  );

  // -- Bidirectional GPIO --------------------------------------------------------------------

  genvar i;
  generate
    for (i = 0; i < 32; i = i + 1) begin : gpio_gen
      assign gpio[i] = gpio_dir[i] ? gpio_out[i] : 1'bz;
    end
  endgenerate
  assign gpio_in = gpio;

  // -- Two-wire and one-wire buses ----------------------------------------------------------

  assign i2c_sda = twi_sda_o ? 1'bz : 1'b0;
  assign i2c_scl = twi_scl_o ? 1'bz : 1'b0;
  assign i2c_sda = twd_sda_o ? 1'bz : 1'b0;
  assign onewire = onewire_o ? 1'bz : 1'b0;

  // -- Serial memory controller: two PSRAMs -------------------------------------------------

  assign psram_data[0] = smc_do;
  assign smc_di = psram_data[1];

  psram_model #(.MEM_BYTES(PSRAM_SIZE)) psram_model_0_inst (
    .sck(smc_clk), .cs_n(smc_csn[0]), .sio(psram_data));
  psram_model #(.MEM_BYTES(PSRAM_SIZE)) psram_model_1_inst (
    .sck(smc_clk), .cs_n(smc_csn[1]), .sio(psram_data));

  // -- SPI and SDI loop-back ----------------------------------------------------------------

  assign spi_di = spi_csn[7] ? spi_do : sdi_do;

  // -- Stream-link FIFO buffer --------------------------------------------------------------

  neorv32_prim_fifo #(.AWIDTH(0), .DWIDTH(32 + 4 + 1), .OUTGATE(1'b1), .ASYNCRD(1'b0))
    slink_buffer (
      .clk_i(clk_gen), .rstn_i(rst_gen), .clear_i(1'b0),
      .wdata_i({slink_tx_last, slink_tx_addr, slink_tx_data}), .we_i(slink_tx_valid),
      .free_o(slink_tx_ready),
      .re_i(slink_rx_ready), .rdata_o({slink_rx_last, slink_rx_addr, slink_rx_data}),
      .avail_o(slink_rx_valid));

  // -- UART simulation receivers ------------------------------------------------------------

  sim_uart_rx #(.NAME("tb.uart0_rx"), .FCLK(100000000.0), .BAUD(19200.0))
    sim_rx_uart0 (.clk(clk_gen), .rxd(uart0_txd));
  sim_uart_rx #(.NAME("tb.uart1_rx"), .FCLK(100000000.0), .BAUD(19200.0))
    sim_rx_uart1 (.clk(clk_gen), .rxd(uart1_txd));

  // -- XBUS interconnect --------------------------------------------------------------------

  xbus_gateway #(
    .DEV_0_EN(1'b0), .DEV_0_SIZE(EXT_MEM_A_SIZE), .DEV_0_BASE(EXT_MEM_A_BASE),
    .DEV_1_EN(1'b0), .DEV_1_SIZE(EXT_MEM_B_SIZE), .DEV_1_BASE(EXT_MEM_B_BASE),
    .DEV_2_EN(1'b1), .DEV_2_SIZE(8), .DEV_2_BASE(32'hF0000000),
    .DEV_3_EN(1'b1), .DEV_3_SIZE(4), .DEV_3_BASE(32'hFF000000),
    .DEV_4_EN(1'b1), .DEV_4_SIZE(16), .DEV_4_BASE(32'hFF100000),
    .DEV_5_EN(1'b1), .DEV_5_SIZE(16), .DEV_5_BASE(32'hFF200000),
    .DEV_6_EN(1'b1), .DEV_6_SIZE(CACHE_BLOCK_SIZE), .DEV_6_BASE(32'hD0000000),
    .DEV_7_EN(1'b1), .DEV_7_SIZE(CACHE_BLOCK_SIZE), .DEV_7_BASE(32'hD1000000)
  ) xbus_interconnect (
    .host_req_i(xbus_core_req), .host_rsp_o(xbus_core_rsp),
    .dev_0_req_o(xbus_ext_mem_a_req), .dev_0_rsp_i(34'd0),
    .dev_1_req_o(xbus_ext_mem_b_req), .dev_1_rsp_i(34'd0),
    .dev_2_req_o(xbus_mmio_req), .dev_2_rsp_i(xbus_mmio_rsp),
    .dev_3_req_o(xbus_trig_req), .dev_3_rsp_i(xbus_trig_rsp),
    .dev_4_req_o(xbus_fmem_data_req), .dev_4_rsp_i(xbus_fmem_data_rsp),
    .dev_5_req_o(xbus_fmem_tag_req), .dev_5_rsp_i(xbus_fmem_tag_rsp),
    .dev_6_req_o(xbus_rom_req), .dev_6_rsp_i(xbus_rom_rsp),
    .dev_7_req_o(xbus_ram_req), .dev_7_rsp_i(xbus_ram_rsp)
  );

  // -- XBUS: external ROM and RAM dummies (cached) ------------------------------------------

  // A request's write enable, byte enables' end, strobe and cycle are its lowest bits.
  always @(negedge rst_gen or posedge clk_gen) begin
    if (!rst_gen) begin
      xbus_rom_rsp <= 34'd0;
      xbus_ram_rsp <= 34'd0;
    end else begin
      xbus_rom_rsp <= 34'd0;
      xbus_ram_rsp <= 34'd0;
      if (xbus_rom_req[0] && xbus_rom_req[1]) // ROM access
        xbus_rom_rsp <= {32'habcd1234, !xbus_rom_req[6], xbus_rom_req[6]}; // error if a write
      else if (xbus_ram_req[0] && xbus_ram_req[1]) // RAM access
        xbus_ram_rsp <= {32'h5678eeff, 1'b1, 1'b0};
    end
  end

  // -- XBUS: external memory-mapped IO (uncached) -------------------------------------------

  xbus_memory #(.MEM_RST(1'b1), .MEM_SIZE(8), .MEM_LATE(16), .MEM_FILE(""))
    xbus_mmio (.clk_i(clk_gen), .rstn_i(rst_gen), .xbus_req_i(xbus_mmio_req),
               .xbus_rsp_o(xbus_mmio_rsp));

  // -- XBUS: external IRQ trigger -----------------------------------------------------------

  always @(negedge rst_gen or posedge clk_gen) begin
    if (!rst_gen) begin
      msi <= 1'b0;
      mti <= 1'b0;
      mei <= 1'b0;
    end else begin
      xbus_trig_rsp <= 34'd0;
      // cycle, strobe, write enable and every byte enable
      if (xbus_trig_req[0] && xbus_trig_req[1] && xbus_trig_req[6] && &xbus_trig_req[5:2]) begin
        xbus_trig_rsp[1] <= 1'b1; // acknowledge
        msi <= xbus_trig_req[13 + 3]; // machine software interrupt, write data bit 3
        mti <= xbus_trig_req[13 + 7]; // machine timer interrupt, write data bit 7
        mei <= xbus_trig_req[13 + 11]; // machine external interrupt, write data bit 11
      end
    end
  end

  // -- XBUS: bus-error test memory ----------------------------------------------------------

  xbus_fmem #(.MEM_SIZE(16)) xbus_fmem_inst (
    .clk_i(clk_gen), .rstn_i(rst_gen), .tag_req_i(xbus_fmem_tag_req),
    .tag_rsp_o(xbus_fmem_tag_rsp), .mem_req_i(xbus_fmem_data_req),
    .mem_rsp_o(xbus_fmem_data_rsp));

endmodule
