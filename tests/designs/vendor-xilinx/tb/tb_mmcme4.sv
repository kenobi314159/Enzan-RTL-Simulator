// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's UltraScale clock manager MMCME4_ADV, which paces its clocks by named events, making 100 MHz and 200 MHz from a 100 MHz input through a
// 1 GHz VCO, its feedback looped back: it locks after its reset, and its outputs then have the
// periods asked for, measured over several cycles once locked.

`timescale 1ns / 1ps

module tb_mmcme4;
    logic clkin = 1'b0;
    always #5 clkin = ~clkin;

    logic rst = 1'b1;
    wire clkfb, clk0, clk1, locked;
    MMCME4_ADV #(.CLKIN1_PERIOD(10.0), .CLKFBOUT_MULT_F(10.0), .DIVCLK_DIVIDE(1),
                 .CLKOUT0_DIVIDE_F(10.0), .CLKOUT1_DIVIDE(5)) u_mmcm (
        .CDDCDONE(), .CDDCREQ(1'b0), .CLKFBOUT(clkfb), .CLKFBOUTB(), .CLKFBSTOPPED(), .CLKINSTOPPED(), .CLKOUT0(clk0),
        .CLKOUT0B(), .CLKOUT1(clk1), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(),
        .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .DO(), .DRDY(), .LOCKED(locked),
        .PSDONE(), .CLKFBIN(clkfb), .CLKIN1(clkin), .CLKIN2(1'b0), .CLKINSEL(1'b1),
        .DADDR(7'd0), .DCLK(1'b0), .DEN(1'b0), .DI(16'd0), .DWE(1'b0), .PSCLK(1'b0),
        .PSEN(1'b0), .PSINCDEC(1'b0), .PWRDWN(1'b0), .RST(rst));

    int failures = 0;
    task automatic period(int which, realtime want);
        realtime first;
        if (which == 0) @(posedge clk0); else @(posedge clk1);
        first = $realtime;
        repeat (8) if (which == 0) @(posedge clk0); else @(posedge clk1);
        $display("clkout%0d: %0.3f ns", which, ($realtime - first) / 8.0);
        if (($realtime - first) / 8.0 != want) failures = failures + 1;
    endtask

    initial begin
        #200 rst = 1'b0;
        wait (locked === 1'b1);
        $display("locked");
        repeat (20) @(posedge clkin);
        period(0, 10.0);
        period(1, 5.0);
        if (failures == 0) $display("PASS tb_mmcme4");
        $finish;
    end

    initial begin
        #200_000 $display("FAIL: no lock by 200 us");
        $finish;
    end
endmodule
