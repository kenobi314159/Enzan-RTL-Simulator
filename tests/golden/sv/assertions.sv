// SPDX-License-Identifier: GPL-3.0-or-later
//
// Immediate assertions (IEEE 1800 section 16.3): an assertion runs its pass action when its
// expression is true and its fail action when it is 0, X or Z, and with no fail action of its
// own a failure is an $error, which reports and carries on.

module tb;
    logic [3:0] value;
    int passed = 0;
    int failed = 0;
    int failures = 0;

    initial begin
        value = 4'd5;
        assert (value == 4'd5) passed = passed + 1; else failed = failed + 1;
        assert (value == 4'd6) passed = passed + 1; else failed = failed + 1;

        // An unknown expression fails the assertion.
        value = 4'bx101;
        assert (value == 4'd5) passed = passed + 1; else failed = failed + 1;

        // A cover runs its action when its expression holds, and never fails.
        value = 4'd2;
        cover (value == 4'd2) passed = passed + 1;
        cover (value == 4'd3) passed = passed + 1;

        $display("passed=%0d failed=%0d", passed, failed);
        if (passed != 2 || failed != 2) begin
            $display("FAIL: assertion actions");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL assertions: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS assertions");
        $finish;
    end
endmodule
