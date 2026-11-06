// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's shift register SRLC32E, on either clock edge, against shift registers of the
// testbench's own: its initial contents, a shift on each enabled edge, reads at addresses that
// change between edges, and its cascade output Q31. Its paths run from bits of its address;
// every check is of a settled value, 2 ns after what it depends on changes, its longest path
// being 1472 ps.
//
// Icarus: without -gspecify, since it applies a path from a bit of the address to all of it.

`timescale 1ns / 1ps

module tb_srlc32;
    // The clock starts once the initial contents have been read: it rises at 75, 85 and so on.
    logic clk = 1'b0;
    initial #70 forever #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1;
    logic [4:0] a = 5'd0;
    wire q_rise, q31_rise, q_fall, q31_fall;
    SRLC32E #(.INIT(32'h1234_abcd)) u_rise (
        .Q(q_rise), .Q31(q31_rise), .A(a), .CE(ce), .CLK(clk), .D(d));
    SRLC32E #(.INIT(32'h8001_7ffe), .IS_CLK_INVERTED(1'b1)) u_fall (
        .Q(q_fall), .Q31(q31_fall), .A(a), .CE(ce), .CLK(clk), .D(d));

    logic [31:0] rise = 32'h1234_abcd, fall = 32'h8001_7ffe;
    always @(posedge clk)
        if (ce) rise <= {rise[30:0], d};
    always @(negedge clk)
        if (ce) fall <= {fall[30:0], d};

    int failures = 0;
    task automatic compare(int which);
        if ({q_rise, q31_rise, q_fall, q31_fall} !== {rise[a], rise[31], fall[a], fall[31]}) begin
            $display("FAIL: check %0d at a=%0d gave %b, should be %b", which, a,
                     {q_rise, q31_rise, q_fall, q31_fall}, {rise[a], rise[31], fall[a], fall[31]});
            failures = failures + 1;
        end
    endtask

    initial begin
        for (int n = 0; n < 32; n = n + 1) begin
            a = 5'(n);
            #2 compare(n);
        end
        for (int step = 0; step < 80; step = step + 1) begin
            @(negedge clk);
            #2;
            d = step % 3 != 1;
            ce = step % 6 != 4;
            a = 5'(step * 7);
            @(posedge clk);
            #2 compare(1000 + step);
        end
        if (failures == 0) $display("PASS tb_srlc32");
        $finish;
    end
endmodule
