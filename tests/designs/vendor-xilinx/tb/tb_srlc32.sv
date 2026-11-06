// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's shift register SRLC32E against a shift register of the testbench's own: its initial
// contents, a shift on each enabled clock, a read at an address that changes between clocks, and
// its cascade output Q31. Every check is of a settled value, well after the model's delays.
//
// Icarus: without -gspecify, since it refuses the model's paths from bits of its address.

`timescale 1ns / 1ps

module tb_srlc32;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1;
    logic [4:0] a = 5'd0;
    wire q32, q31;
    SRLC32E #(.INIT(32'h1234abcd)) u32 (.Q(q32), .Q31(q31), .A(a), .CE(ce), .CLK(clk), .D(d));

    logic [31:0] shadow32 = 32'h1234abcd;
    always @(posedge clk)
        if (ce) shadow32 <= {shadow32[30:0], d};

    int failures = 0;
    task automatic compare(int which);
        if (q32 !== shadow32[a] || q31 !== shadow32[31]) begin
            $display("FAIL: check %0d at a=%0d gave %b %b", which, a, q32, q31);
            failures = failures + 1;
        end
    endtask

    initial begin
        // Initial contents, read at every address before anything shifts.
        for (int address = 0; address < 32; address++) begin
            a = address[4:0];
            #0.1 compare(address);
        end

        for (int step = 0; step < 80; step++) begin
            @(negedge clk);
            d = step[0] ^ step[2];
            ce = step % 7 != 3;
            a = 5'(step * 11);
            #1 compare(100 + step);
        end
        $display("q32=%b q31=%b", q32, q31);

        if (failures == 0) $display("PASS tb_srlc32");
        $finish;
    end
endmodule
