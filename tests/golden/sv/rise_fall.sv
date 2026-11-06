// SPDX-License-Identifier: GPL-3.0-or-later
//
// Separate rise, fall and turn-off delays of a continuous assignment and of a gate (IEEE 1800
// section 28.16), as AMD's pull-ups and its DCI reset give them: a value of 1 takes the rise
// delay, 0 the fall, z the turn-off, the shorter of the other two when only two are given, and x
// the shortest of all. A pulse shorter than the delay its value calls for never arrives.

`timescale 1ns / 1ns

module tb;
    logic a = 1'b0, en = 1'b1;
    wire y2, y3, g;
    assign #(3, 7) y2 = a;
    assign #(3, 7, 5) y3 = en ? a : 1'bz;
    bufif1 #(2, 6) (g, a, en);

    always @(y2) $display("%0d y2=%b", $time, y2);
    always @(y3) $display("%0d y3=%b", $time, y3);
    always @(g) $display("%0d g=%b", $time, g);

    initial begin
        #20 a = 1'b1;
        #20 a = 1'b0;
        #20 a = 1'bx;
        #20 a = 1'b1;
        #20 en = 1'b0;
        #20 en = 1'b1;
        #20 a = 1'b0;
        #2 a = 1'b1;
        #20 $display("PASS rise_fall");
        $finish;
    end
endmodule
