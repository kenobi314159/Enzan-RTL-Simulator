// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unpacked arrays of reals, as Altera's PLL models keep their clocks' phases and duty cycles:
// elements a function writes, read in arithmetic, as a delay and by a continuous assignment, at
// indexes known only as the design runs, and in two dimensions. Copies and comparisons of whole
// arrays, which Icarus Verilog does not run, are real_aggregates.sv's.

`timescale 1ns / 1ps

`define CHECK(condition, what) \
    if (!(condition)) begin \
        $display("FAIL %s", what); \
        failures++; \
    end

module tb;
    localparam int N = 4;

    real phase[N-1:0];
    real duty[0:N-1];
    real grid[2][3];
    logic out = 1'b0;
    wire first_is_zero;
    int failures = 0;

    function real phase_of(int index);
        return index * 0.25;
    endfunction

    assign first_is_zero = phase[0] == 0.0;

    initial begin
        for (int i = 0; i < N; i++) begin
            phase[i] = phase_of(i);
            duty[i] = 50.0 * (i + 1) / 100.0;
        end
        #1;
        `CHECK(first_is_zero === 1'b1, "a continuous assignment reading an element")
        `CHECK(phase[3] == 0.75, "an element a function wrote")
        $display("phase %f %f, duty %f %f", phase[1], phase[3], duty[0], duty[3]);

        // A delay of an element's value, 1 ns.
        out <= #(duty[1]) 1'b1;
        #0.5 `CHECK(out === 1'b0, "a delay of an element, before it")
        #0.6 `CHECK(out === 1'b1, "a delay of an element, after it")

        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 3; j++) grid[i][j] = i * 10.0 + j / 2.0;
        `CHECK(grid[1][2] == 11.0, "an element of two dimensions")
        `CHECK(grid[0][1] + grid[1][0] == 10.5, "elements of two dimensions in arithmetic")
        $display("grid %f %f", grid[0][1], grid[1][2]);

        if (failures == 0) $display("PASS real_arrays");
        $finish;
    end
endmodule
