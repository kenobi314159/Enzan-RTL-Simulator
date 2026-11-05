// SPDX-License-Identifier: GPL-3.0-or-later
//
// SystemVerilog's `real` (IEEE 1800 sections 6.12 and 20.5), as AMD's clock managers compute with
// it: variables and parameters, arithmetic and comparison, conversion into integers by rounding
// and by `$rtoi`'s truncation, the bits of a double, the time in fractions of a unit, a clock whose
// half period is a real known only as it runs, and the real conversions of `$display`.

`timescale 1ns / 1ps

module tb;
    parameter real Period = 2.5;
    localparam real Half = Period / 2.0;

    real a = 1.5, b = -2.25, zero = 0.0;
    real sum, product, quotient;
    real watched = 0.0;
    int changes = 0;
    integer rounded, truncated;
    logic [63:0] bits;

    // A clock whose half period is a variable's, in the module's nanoseconds.
    real half_period = 1.25;
    logic clk = 1'b0;
    int edges = 0;
    always begin
        #(half_period) clk = ~clk;
        edges = edges + 1;
    end

    always @(watched) changes = changes + 1;

    function automatic real average(real x, real y);
        return (x + y) / 2.0;
    endfunction

    int failures = 0;

    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures = failures + 1;
        end
    endtask

    initial begin
        sum = a + b;
        product = a * b;
        quotient = a / b;
        $display("sum=%f product=%f quotient=%f", sum, product, quotient);
        $display("%e %g %0.3f %8.2f|%-8.2f|", product, quotient, Half, b, a);
        check(1, sum == -0.75);
        check(2, product < b && b < a && a >= 1.5);
        check(3, average(a, b) == -0.375);
        check(4, Half == 1.25);

        // An assignment rounds halfway away from zero; $rtoi drops the fraction.
        rounded = 2.5;
        truncated = $rtoi(2.7);
        $display("rounded=%0d truncated=%0d", rounded, truncated);
        check(5, rounded == 3);
        check(6, truncated == 2);
        rounded = -2.5;
        truncated = $rtoi(-2.7);
        check(7, rounded == -3 && truncated == -2);
        check(8, $itor(7) / 2 == 3.5);
        check(9, int'(a * 3.0) == 5);

        bits = $realtobits(a);
        $display("bits=%h back=%f", bits, $bitstoreal(bits));
        check(10, bits == 64'h3ff8000000000000);
        check(11, $bitstoreal(64'hc002000000000000) == b);

        // The truth of a real is whether it is 0.0, -0.0 included.
        check(12, a && !zero);
        check(13, !(-zero));
        if (zero) check(14, 0);
        check(15, (b ? 1 : 0) == 1);

        // The time in nanoseconds, a fraction of one included.
        #0.75;
        $display("realtime=%f time=%0d", $realtime, $time);
        check(16, $realtime == 0.75);

        // A change of a real wakes what watches it.
        watched = 1.0;
        #1 watched = 1.0;
        #1 watched = -1.0;
        #1 $display("changes=%0d", changes);
        check(17, changes == 2);

        // Twenty half periods of 1.25 ns have passed by 25.75 ns, from 0.
        #22;
        $display("edges=%0d at %f", edges, $realtime);
        check(18, edges == 20);
        // The wait under way keeps its 1.25 ns; the next ones are 0.5 ns, so by 28.15 ns the
        // edges at 26.25, 26.75, 27.25 and 27.75 ns have come.
        half_period = 0.5;
        #2.4 $display("edges=%0d", edges);
        check(19, edges == 24);

        $display("default %f %d", a, b);
        if (failures == 0) $display("PASS reals");
        $finish;
    end
endmodule
