// SPDX-License-Identifier: GPL-3.0-or-later
//
// `wait (condition)` (IEEE 1800 section 9.4.3): a condition that already holds lets the process
// through at once; otherwise the process waits until a change to something the condition reads
// makes it true. An unknown condition is not true, so it keeps waiting.

module tb;
    int   count = 0;
    logic ready = 1'bx;
    logic go = 1'bx;
    int   order [4];
    int   next = 0;
    int   failures = 0;

    always #1 count = count + 1;
    initial #3 ready = 1'b1;
    initial #6 go = 1'b1;

    initial begin
        wait (1'b1) order[next++] = 0;
        wait (count >= 4) order[next++] = $time;
        wait (ready && go) order[next++] = $time;
        wait (count == 2 || count >= 9) order[next++] = $time;

        $display("passed at %0d %0d %0d %0d", order[0], order[1], order[2], order[3]);
        if (order[0] != 0 || order[1] != 4 || order[2] != 6 || order[3] != 9) begin
            $display("FAIL: wait let the process through at the wrong times");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL wait: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS wait");
        $finish;
    end
endmodule
