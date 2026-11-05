// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drive strengths, as two classes (IEEE 1800 section 28.11): a strong driver decides a bit, and a
// weak one only where every strong driver leaves it `z`.
//
// - `bus` has a weak driver and a strong one that releases it.
// - `gsr` is AMD's `glbl` reset: strong when 1, weak when 0, so a strong 0 elsewhere would win.
// - `lock` is its PLLs' lock: each PLL drives a strong 0 while unlocked and a weak 1 once locked,
//   beside a weak 1 the declaration drives, so it is 1 only when every PLL is.
// - `pulled` is a `tri1` whose one driver releases it to the pull.

module tb;
    logic weak_value = 1'b1;
    logic strong_value = 1'bz;
    wire bus;
    assign (weak1, weak0) bus = weak_value;
    assign bus = strong_value;

    logic gsr_int = 1'b1;
    logic gsr_override = 1'bz;
    wire gsr;
    assign (strong1, weak0) gsr = gsr_int;
    assign gsr = gsr_override;

    tri1 pull_up;
    logic locked_a = 1'b0, locked_b = 1'b0;
    tri (weak1, strong0) lock = pull_up;
    assign (weak1, strong0) lock = locked_a;
    assign (weak1, strong0) lock = locked_b;

    logic release_it = 1'b0;
    tri1 pulled;
    assign pulled = release_it ? 1'bz : 1'b0;

    int failures = 0;

    task automatic check(int which, logic actual, logic expected);
        if (actual !== expected) begin
            $display("FAIL: check %0d gave %b, should be %b", which, actual, expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #1 $display("bus=%b gsr=%b lock=%b pulled=%b", bus, gsr, lock, pulled);
        check(1, bus, 1'b1);
        check(2, gsr, 1'b1);
        check(3, lock, 1'b0);
        check(4, pulled, 1'b0);

        strong_value = 1'b0;
        gsr_int = 1'b0;
        locked_a = 1'b1;
        release_it = 1'b1;
        #1 $display("bus=%b gsr=%b lock=%b pulled=%b", bus, gsr, lock, pulled);
        check(5, bus, 1'b0);
        check(6, gsr, 1'b0);
        check(7, lock, 1'b0);
        check(8, pulled, 1'b1);

        weak_value = 1'b0;
        strong_value = 1'b1;
        gsr_override = 1'b1;
        locked_b = 1'b1;
        #1 $display("bus=%b gsr=%b lock=%b pulled=%b", bus, gsr, lock, pulled);
        check(9, bus, 1'b1);
        check(10, gsr, 1'b1);
        check(11, lock, 1'b1);

        gsr_int = 1'b1;
        gsr_override = 1'b0;
        locked_a = 1'b0;
        #1 $display("bus=%b gsr=%b lock=%b pulled=%b", bus, gsr, lock, pulled);
        check(12, gsr, 1'bx);
        check(13, lock, 1'b0);

        if (failures == 0) $display("PASS strengths");
        $finish;
    end
endmodule
