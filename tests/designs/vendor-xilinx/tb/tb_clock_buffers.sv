// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's global clock buffers: BUFG passes every edge of its input, and BUFGCE only while its
// enable is high, the enable changed while the clock is low so that no edge is cut short.

`timescale 1ns / 1ps

module tb_clock_buffers;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic ce = 1'b1;
    wire passed, gated;
    BUFG u_bufg (.O(passed), .I(clk));
    BUFGCE #(.SIM_DEVICE("7SERIES")) u_bufgce (.O(gated), .CE(ce), .I(clk));

    int passedRises = 0, gatedRises = 0;
    always @(posedge passed) passedRises = passedRises + 1;
    always @(posedge gated) gatedRises = gatedRises + 1;

    int failures = 0;
    initial begin
        #200;    // past glbl's reset
        @(negedge clk);
        passedRises = 0;
        gatedRises = 0;
        repeat (10) @(negedge clk);
        $display("enabled: bufg %0d bufgce %0d", passedRises, gatedRises);
        if (passedRises != 10 || gatedRises != 10) failures = failures + 1;

        #2 ce = 1'b0;
        passedRises = 0;
        gatedRises = 0;
        repeat (10) @(negedge clk);
        $display("disabled: bufg %0d bufgce %0d gated=%b", passedRises, gatedRises, gated);
        if (passedRises != 10 || gatedRises > 1 || gated !== 1'b0) failures = failures + 1;

        #2 ce = 1'b1;
        gatedRises = 0;
        repeat (10) @(negedge clk);
        $display("enabled again: bufgce %0d", gatedRises);
        if (gatedRises < 9) failures = failures + 1;

        if (failures == 0) $display("PASS tb_clock_buffers");
        $finish;
    end
endmodule
