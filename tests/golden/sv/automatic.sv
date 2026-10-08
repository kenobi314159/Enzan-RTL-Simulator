// SPDX-License-Identifier: GPL-3.0-or-later
//
// An automatic variable declared in a block of a static process (IEEE 1800 section 6.21): it
// starts again on every entry to the block, from an initialiser that need not be constant, where
// a static one is set once at time zero. Icarus Verilog does not implement the override, so the
// case checks itself.

module tb;
    int sum;
    int failures = 0;

    initial begin
        sum = 0;
        for (int i = 1; i <= 3; i = i + 1) begin
            automatic int doubled = i * 2;
            static int entries = 0;
            entries = entries + 1;
            for (int j = i + 1; j <= 3; j = j + 1) sum = sum + doubled;
            if (i == 3 && entries != 3) begin
                $display("FAIL: a static variable was set again, entries is %0d", entries);
                failures = failures + 1;
            end
        end
        if (sum != 8) begin
            $display("FAIL: automatic block variables, sum is %0d", sum);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL automatic: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS automatic");
        $finish;
    end
endmodule
