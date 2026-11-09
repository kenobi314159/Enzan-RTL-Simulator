// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's shift registers SRL16E, on either clock edge, and SRLC16, against shift registers of
// the testbench's own: their initial contents, a shift on each enabled edge, reads at addresses
// that change between edges, and the delays of their paths, 1472 ps from the clock, 1114 ps to
// SRLC16's Q15, and from the address 631 ps for A0 and 238 ps for A3.

`timescale 1ns / 1ps

module tb_shift;
    // The clock starts once the initial contents have been read: it rises at 35, 45 and so on.
    logic clk = 1'b0;
    initial #30 forever #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1;
    logic [3:0] a = 4'd0;
    wire q_rise, q_fall, q_c, q15;
    SRL16E #(.INIT(16'h1234)) u_rise (
        .Q(q_rise), .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .CE(ce), .CLK(clk), .D(d));
    SRL16E #(.INIT(16'hfe01), .IS_CLK_INVERTED(1'b1)) u_fall (
        .Q(q_fall), .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .CE(ce), .CLK(clk), .D(d));
    SRLC16 #(.INIT(16'h8421)) u_c (
        .Q(q_c), .Q15(q15), .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .CLK(clk), .D(d));

    logic [15:0] rise = 16'h1234, fall = 16'hfe01, plain = 16'h8421;
    always @(posedge clk) begin
        if (ce) rise <= {rise[14:0], d};
        plain <= {plain[14:0], d};
    end
    always @(negedge clk)
        if (ce) fall <= {fall[14:0], d};

    int failures = 0;
    task automatic compare(int which);
        if ({q_rise, q_fall, q_c, q15} !== {rise[a], fall[a], plain[a], plain[15]}) begin
            $display("FAIL: check %0d at a=%0d gave %b, should be %b", which, a,
                     {q_rise, q_fall, q_c, q15}, {rise[a], fall[a], plain[a], plain[15]});
            failures = failures + 1;
        end
    endtask
    task automatic expect1(int which, logic got, logic want);
        $display("%.3f: check %0d q=%b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    // The inputs change 2 ns after a falling edge, and the outputs are compared 2 ns after each
    // edge, by when every path's delay has run out.
    initial begin
        #2 for (int n = 0; n < 16; n = n + 1) begin
            a = n[3:0];
            #1 compare(n);
        end
        for (int step = 0; step < 40; step = step + 1) begin
            @(negedge clk);
            #2;
            d = step % 3 == 1;
            ce = step % 7 != 3;
            a = 4'(step * 5);
            #1 compare(100 + step);
            @(posedge clk);
            #2 compare(200 + step);
        end

        // Alternating bits, so that each rising edge changes Q at address 0 and Q15, and Q
        // between neighbouring addresses differs.
        ce = 1'b1;
        a = 4'd0;
        for (int k = 0; k < 17; k = k + 1) begin
            @(negedge clk);
            #2 d = k[0];
        end

        // From the clock: the rising edge's shift reaches Q15 1114 ps on, and Q 1472 ps on.
        begin
            logic q_before, q_after, q15_before, q15_after;
            q_before = rise[0];
            q_after = d;
            q15_before = plain[15];
            q15_after = plain[14];
            @(posedge clk);
            #1.112 expect1(1, q15, q15_before);
            #0.004 expect1(2, q15, q15_after);
            #0.354 expect1(3, q_rise, q_before);
            #0.004 expect1(4, q_rise, q_after);
        end

        // From the address: A0 631 ps, and A0 and A3 at once A3's 238 ps.
        #1 a = 4'd1;
        #0.629 expect1(5, q_rise, rise[0]);
        #0.004 expect1(6, q_rise, rise[1]);
        a = 4'd8;
        #0.236 expect1(7, q_rise, rise[1]);
        #0.004 expect1(8, q_rise, rise[8]);

        if (failures == 0) $display("PASS tb_shift");
        $finish;
    end
endmodule
