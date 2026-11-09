// SPDX-License-Identifier: GPL-3.0-or-later
//
// A parallel module path from one bit to a vector, `(posedge clk => (q : d))` with q four bits
// wide, as Yosys's block RAMs and Altera's atoms write it: IEEE 1800 section 30.4.5 asks for
// terminals of one width, and Icarus Verilog refuses it, but the bit can only join every bit of
// the output, so it is accepted as a warning and delays each bit as a full path would. The case
// checks itself.

`timescale 1ns / 1ns

module word (input clk, input [3:0] d, output reg [3:0] q);
    always @(posedge clk) q <= d;
    specify
        (posedge clk => (q : d)) = 3;
    endspecify
endmodule

module tb;
    logic clk = 1'b0;
    logic [3:0] d = 4'h0;
    wire [3:0] q;
    word u_word (.clk(clk), .d(d), .q(q));

    int failures = 0;
    task automatic expect4(int which, logic [3:0] want);
        $display("%0d: check %0d q=%h", $time, which, q);
        if (q !== want) begin
            $display("FAIL: check %0d gave %h, should be %h", which, q, want);
            failures = failures + 1;
        end
    endtask

    initial begin
        #10 clk = 1'b1;            // q from x to 0, 3 after the edge
        #2 expect4(1, 4'hx);
        #2 expect4(2, 4'h0);
        clk = 1'b0;
        d = 4'h9;
        #10 clk = 1'b1;            // every bit of q, 3 after the edge
        #2 expect4(3, 4'h0);
        #2 expect4(4, 4'h9);
        if (failures == 0) $display("PASS one_bit_parallel_path");
        $finish;
    end
endmodule
