// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's flip-flops and latches: FDRE, FDSE, FDCE, FDPE and FDRE_1, one with its clock and reset
// inverted by parameter, and LDCE and LDPE. Their initial values, a clock enable, a synchronous
// reset and set, an asynchronous clear and preset, the falling edge, and the models' delays: 303
// ps from a clock edge, the conditions of their paths choosing the edge, and 764 ps from a clear
// or preset.

`timescale 1ns / 1ps

module tb_flops;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1, r = 1'b0, s = 1'b0, clr = 1'b0, pre = 1'b0;
    wire q_re, q_se, q_ce, q_pe, q_neg, q_inv;
    FDRE #(.INIT(1'b1)) u_fdre (.Q(q_re), .C(clk), .CE(ce), .D(d), .R(r));
    FDSE #(.INIT(1'b0)) u_fdse (.Q(q_se), .C(clk), .CE(ce), .D(d), .S(s));
    FDCE #(.INIT(1'b1)) u_fdce (.Q(q_ce), .C(clk), .CE(ce), .D(d), .CLR(clr));
    FDPE #(.INIT(1'b0)) u_fdpe (.Q(q_pe), .C(clk), .CE(ce), .D(d), .PRE(pre));
    FDRE_1 #(.INIT(1'b0)) u_fdre_1 (.Q(q_neg), .C(clk), .CE(ce), .D(d), .R(r));
    FDRE #(.IS_C_INVERTED(1'b1), .IS_R_INVERTED(1'b1)) u_inverted (
        .Q(q_inv), .C(clk), .CE(ce), .D(d), .R(~r));

    logic g = 1'b0, ge = 1'b1, lclr = 1'b0, lpre = 1'b0;
    wire l_ce, l_pe;
    LDCE #(.INIT(1'b1)) u_ldce (.Q(l_ce), .CLR(lclr), .D(d), .G(g), .GE(ge));
    LDPE #(.INIT(1'b0)) u_ldpe (.Q(l_pe), .PRE(lpre), .D(d), .G(g), .GE(ge));

    int failures = 0;
    task automatic expect6(int which, logic [5:0] want);
        logic [5:0] got;
        got = {q_re, q_se, q_ce, q_pe, q_neg, q_inv};
        $display("%.3f: check %0d flops=%b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask
    task automatic expect_latches(int which, logic [1:0] want);
        $display("%.3f: check %0d latches=%b", $realtime, which, {l_ce, l_pe});
        if ({l_ce, l_pe} !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, {l_ce, l_pe}, want);
            failures = failures + 1;
        end
    endtask

    // The clock rises at 5, 15, 25 and so on, and falls at 10, 20 and so on. Each check falls
    // between edges, after the outputs' delays, but where it is of a delay.
    initial begin
        #2 expect6(1, 6'b101000);              // the initial values, the inverted one's 0
        expect_latches(1, 2'b10);
        #5 expect6(2, 6'b000000);              // rising at 5, clocked in d = 0
        d = 1'b1;
        #5 expect6(3, 6'b000011);              // falling at 10: FDRE_1 and the inverted one
        #5 expect6(4, 6'b111111);              // rising at 15
        ce = 1'b0;
        d = 1'b0;
        #10 expect6(5, 6'b111111);             // disabled, kept
        ce = 1'b1;
        r = 1'b1;
        s = 1'b1;
        d = 1'b1;
        #10 expect6(6, 6'b011100);             // reset and set, at 30 and 35
        r = 1'b0;
        s = 1'b0;
        d = 1'b0;
        #10 expect6(7, 6'b000000);
        d = 1'b1;
        #5 expect6(8, 6'b000011);              // at 52, after the falling edge
        pre = 1'b1;
        #0.7 expect6(9, 6'b000011);            // the preset 764 ps on
        #0.1 expect6(10, 6'b000111);
        #0.2 pre = 1'b0;
        #4 expect6(11, 6'b111111);             // at 57, after the rising edge
        clr = 1'b1;
        #0.7 expect6(12, 6'b111111);           // the clear 764 ps on
        #0.1 expect6(13, 6'b110111);
        #0.2 clr = 1'b0;
        d = 1'b0;

        // From a falling and a rising edge, 303 ps.
        @(negedge clk);
        #0.3 expect6(14, 6'b110111);
        #0.01 expect6(15, 6'b110100);
        @(posedge clk);
        #0.3 expect6(16, 6'b110100);
        #0.01 expect6(17, 6'b000000);

        // The latches: open, closed, cleared and preset at once, and closed by the gate enable.
        #1 d = 1'b1;
        g = 1'b1;
        #1 expect_latches(2, 2'b11);
        g = 1'b0;
        d = 1'b0;
        #1 expect_latches(3, 2'b11);
        lclr = 1'b1;
        #1 expect_latches(4, 2'b01);
        lclr = 1'b0;
        g = 1'b1;
        #1 expect_latches(5, 2'b00);
        lpre = 1'b1;
        #1 expect_latches(6, 2'b01);
        ge = 1'b0;
        lpre = 1'b0;
        d = 1'b1;
        #1 expect_latches(7, 2'b01);

        if (failures == 0) $display("PASS tb_flops");
        $finish;
    end
endmodule
