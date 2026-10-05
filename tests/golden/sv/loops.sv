// SPDX-License-Identifier: GPL-3.0-or-later
//
// `break` and `continue` in every kind of loop (IEEE 1800 section 12.8), and `foreach` over
// fixed dimensions (section 12.7.3): each iterator runs from its dimension's left bound to its
// right, the first dimension outermost; a skipped dimension takes no loop; and `continue` moves
// the innermost iterator on. `break` in a `foreach` of two dimensions is foreach_break.sv.

module tb;
    int sum;
    int count;
    int seen;
    int grid [0:2][3:0];
    logic [7:0] down [3:0];
    logic [3:0][1:0] packed_pairs;
    int failures = 0;

    initial begin
        sum = 0;
        for (int i = 0; i < 10; i++) begin
            if (i == 2) continue;
            if (i == 6) break;
            sum += i;
        end
        $display("for: %0d", sum);

        count = 0;
        sum = 0;
        while (1) begin
            count++;
            if (count % 2 == 0) continue;
            if (count > 7) break;
            sum += count;
        end
        $display("while: %0d after %0d", sum, count);

        count = 0;
        do begin
            count++;
            if (count == 3) continue;
        end while (count < 5);
        $display("do-while: %0d", count);

        sum = 0;
        repeat (6) begin
            sum++;
            if (sum == 2) continue;
            if (sum == 4) break;
        end
        $display("repeat: %0d", sum);

        count = 0;
        forever begin
            count++;
            if (count == 3) break;
        end
        $display("forever: %0d", count);

        foreach (grid[i, j]) grid[i][j] = i * 10 + j;
        seen = 0;
        foreach (grid[i, j]) begin
            if (j == 2) continue;
            $display("grid[%0d][%0d] = %0d", i, j, grid[i][j]);
            seen++;
        end
        $display("foreach: %0d seen", seen);

        foreach (down[k]) down[k] = 8'(k);
        foreach (down[k]) $display("down[%0d] = %0d", k, down[k]);

        foreach (grid[, j]) $display("column %0d", j);

        packed_pairs = 8'b11_10_01_00;
        foreach (packed_pairs[p]) $display("pair %0d = %b", p, packed_pairs[p]);

        if (seen != 9 || grid[2][3] != 23 || down[3] != 8'd3) begin
            $display("FAIL: foreach visited %0d elements", seen);
            failures = failures + 1;
        end
        if (failures != 0) $fatal(1);
        $display("PASS loops");
        $finish;
    end
endmodule
