// SPDX-License-Identifier: GPL-3.0-or-later
//
// A SystemVerilog testbench around a VHDL unit (nesting.vhd) that is made of SystemVerilog
// registers: a hierarchy that crosses into VHDL and back, at two widths.
`timescale 1ns / 1ps

module Stage #(
    parameter int N = 1
) (
    input logic clk,
    input logic [N-1:0] d,
    output logic [N-1:0] q
);
    always_ff @(posedge clk) q <= d;
endmodule

module tb;
    logic clk = 1'b0;
    logic [2:0] narrowIn = 3'd0;
    logic [2:0] narrowOut;
    logic [7:0] wideIn = 8'd0;
    logic [7:0] wideOut;

    Delay #(.Width(3)) narrow (.Clk(clk), .D(narrowIn), .Q(narrowOut));
    Delay #(.Width(8)) wide (.Clk(clk), .D(wideIn), .Q(wideOut));

    always #5 clk = ~clk;

    initial begin
        for (int unsigned step = 1; step <= 6; step++) begin
            @(negedge clk);
            narrowIn = 3'(step);
            wideIn = 8'(step * 37);
            $display("%0d: narrow %0d, wide %0d", $time, narrowOut, wideOut);
        end
        if (narrowOut == 3'd4 && wideOut == 8'd148) $display("PASS nesting");
        else $display("FAIL nesting");
        $finish;
    end
endmodule
