// SPDX-License-Identifier: GPL-3.0-or-later
//
// Separate rise, fall and turn-off delays on a vector continuous assignment (IEEE 1800 section
// 10.3.3). A vector becoming zero takes the fall delay, one becoming all z takes the turn-off
// delay, and every other transition takes the rise delay.

`timescale 1ns / 1ns

module tb;
    logic [3:0] value = 4'b0000;
    wire [3:0] delayed;
    assign #(3, 7, 5) delayed = value;

    initial begin
        // A mixture of transitions is the vector's "all other" case and takes the rise delay.
        #20 value = 4'b10xz;
        #4 if (delayed !== 4'b10xz) $fatal(1, "vector rise delay: %b", delayed);

        value = 4'b0000;
        #6 if (delayed !== 4'b10xz) $fatal(1, "early vector fall: %b", delayed);
        #2 if (delayed !== 4'b0000) $fatal(1, "vector fall delay: %b", delayed);

        value = 4'bzzzz;
        #4 if (delayed !== 4'b0000) $fatal(1, "early vector turn-off: %b", delayed);
        #2 if (delayed !== 4'bzzzz) $fatal(1, "vector turn-off delay: %b", delayed);

        $display("PASS vector_transition_delays");
        $finish;
    end
endmodule
