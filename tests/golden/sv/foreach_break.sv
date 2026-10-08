// SPDX-License-Identifier: GPL-3.0-or-later
//
// `break` in a `foreach` over two dimensions leaves the whole statement, which is one loop
// however many dimensions it iterates (IEEE 1800 section 12.8). Verilator agrees; Icarus
// Verilog leaves only the innermost dimension's loop, as if the statement were nested loops.

module tb;
    int grid [0:2][3:0];
    int count = 0;

    initial begin
        foreach (grid[i, j]) begin
            if (i == 1 && j == 1) break;
            count++;
        end
        $display("counted %0d", count);
        if (count != 6) begin
            $display("FAIL: break left only the inner dimension, after %0d", count);
            $fatal(1);
        end
        $display("PASS foreach_break");
        $finish;
    end
endmodule
