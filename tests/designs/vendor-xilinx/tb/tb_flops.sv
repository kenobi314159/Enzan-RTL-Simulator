// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's flip-flops FDRE, FDCE, FDPE and FDSE: their initial values held while glbl's reset is
// high, then a clock enable, a synchronous reset and set, an asynchronous clear and preset, and
// the 100 ps clock-to-output delay of their models.

`timescale 1ns / 1ps

module tb_flops;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1, r = 1'b0, s = 1'b0, clr = 1'b0, pre = 1'b0;
    wire q_re, q_ce, q_pe, q_se;
    FDRE #(.INIT(1'b1)) u_fdre (.Q(q_re), .C(clk), .CE(ce), .D(d), .R(r));
    FDCE #(.INIT(1'b1)) u_fdce (.Q(q_ce), .C(clk), .CE(ce), .D(d), .CLR(clr));
    FDPE #(.INIT(1'b0)) u_fdpe (.Q(q_pe), .C(clk), .CE(ce), .D(d), .PRE(pre));
    FDSE #(.INIT(1'b0)) u_fdse (.Q(q_se), .C(clk), .CE(ce), .D(d), .S(s));

    int failures = 0;
    task automatic expect4(int which, logic [3:0] got, logic [3:0] want);
        $display("%0d: check %0d re=%b ce=%b pe=%b se=%b", $time, which, got[3], got[2], got[1],
                 got[0]);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    // Each check falls between clock edges, after the outputs' delay.
    initial begin
        #50 expect4(1, {q_re, q_ce, q_pe, q_se}, 4'b1100);    // held at INIT by GSR
        #100 expect4(2, {q_re, q_ce, q_pe, q_se}, 4'b0000);   // released, clocked in d = 0
        d = 1'b1;
        #10 expect4(3, {q_re, q_ce, q_pe, q_se}, 4'b1111);
        ce = 1'b0;
        d = 1'b0;
        #10 expect4(4, {q_re, q_ce, q_pe, q_se}, 4'b1111);    // disabled, kept
        ce = 1'b1;
        r = 1'b1;
        s = 1'b1;
        d = 1'b1;
        #10 expect4(5, {q_re, q_ce, q_pe, q_se}, 4'b0111);    // reset and set at the edge
        r = 1'b0;
        s = 1'b0;
        clr = 1'b1;
        #1 expect4(6, {q_re, q_ce, q_pe, q_se}, 4'b0011);     // cleared at once, no edge
        clr = 1'b0;
        d = 1'b0;
        #9 pre = 1'b1;
        #1 expect4(7, {q_re, q_ce, q_pe, q_se}, 4'b0010);     // preset at once
        pre = 1'b0;

        // The clock-to-output delay: just before 100 ps after an edge the outputs are the old
        // values, FDPE's still the preset, and just after it the new ones.
        d = 1'b1;
        @(posedge clk);
        #0.09 expect4(8, {q_re, q_ce, q_pe, q_se}, 4'b0010);
        #0.02 expect4(9, {q_re, q_ce, q_pe, q_se}, 4'b1111);

        if (failures == 0) $display("PASS tb_flops");
        $finish;
    end
endmodule
