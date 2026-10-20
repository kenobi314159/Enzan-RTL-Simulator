// SPDX-License-Identifier: GPL-3.0-or-later
//
// A SystemVerilog testbench around three instances of a VHDL counter (generics.vhd): one with every
// generic's default, one whose width and step parameters set, and one whose boolean a bit clears.
`timescale 1ns / 1ps

module tb;
    logic clk = 1'b0;
    logic rst = 1'b1;
    logic [3:0] plain;
    logic [5:0] stepped;
    logic [3:0] stopped;
    int plainTotal, steppedTotal, stoppedTotal;
    bit plainWrapped, steppedWrapped, stoppedWrapped;

    Counter u_plain (.Clk(clk), .Rst(rst), .Count(plain), .Total(plainTotal),
                     .Wrapped(plainWrapped));
    Counter #(.Width(6), .Step(5)) u_stepped (
        .Clk(clk), .Rst(rst), .Count(stepped), .Total(steppedTotal), .Wrapped(steppedWrapped));
    Counter #(.Enabled(1'b0)) u_stopped (.Clk(clk), .Rst(rst), .Count(stopped),
                                         .Total(stoppedTotal), .Wrapped(stoppedWrapped));

    always #5 clk = ~clk;

    initial begin
        @(negedge clk) rst = 1'b0;
        repeat (20) begin
            @(negedge clk);
            $display("%0d: plain %0d %0d %b, stepped %0d %0d %b, stopped %0d %0d %b", $time,
                     plain, plainTotal, plainWrapped, stepped, steppedTotal, steppedWrapped,
                     stopped, stoppedTotal, stoppedWrapped);
        end
        if (plain == 4'd4 && stepped == 6'd36 && stopped == 4'd0 && stoppedTotal == 20)
            $display("PASS generics");
        else $display("FAIL generics");
        $finish;
    end
endmodule
