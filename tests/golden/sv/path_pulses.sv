// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module path's pulse limits (IEEE 1800 section 30.7): by default a pulse shorter than the
// path's delay is rejected, and `PATHPULSE$ = 0`, which AMD's models set, lets every pulse
// through, each change arriving its delay after it was made. Icarus Verilog ignores
// `PATHPULSE$`, so the case checks itself against the standard.

`timescale 1ns / 1ns

module rejecting (input a, output y);
    assign y = a;
    specify
        (a => y) = 5;
    endspecify
endmodule

module passing (input a, output y);
    assign y = a;
    specify
        (a => y) = 5;
        specparam PATHPULSE$ = 0;
    endspecify
endmodule

module tb;
    logic a = 1'b0;
    wire rejected, passed;
    rejecting u_rejecting (.a(a), .y(rejected));
    passing u_passing (.a(a), .y(passed));

    int rejectedChanges = 0, passedChanges = 0;
    always @(rejected) rejectedChanges = rejectedChanges + 1;
    always @(passed) passedChanges = passedChanges + 1;

    int failures = 0;
    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures = failures + 1;
        end
    endtask

    initial begin
        #20;
        check(1, rejected === 1'b0 && passed === 1'b0);
        rejectedChanges = 0;
        passedChanges = 0;

        // A pulse of 2 against a delay of 5.
        a = 1'b1;
        #2 a = 1'b0;
        #4 check(2, passed === 1'b1 && rejected === 1'b0);
        #2 check(3, passed === 1'b0);
        #20 $display("rejected %0d passed %0d", rejectedChanges, passedChanges);
        check(4, rejectedChanges == 0);
        check(5, passedChanges == 2);

        if (failures == 0) $display("PASS path_pulses");
        $finish;
    end
endmodule
