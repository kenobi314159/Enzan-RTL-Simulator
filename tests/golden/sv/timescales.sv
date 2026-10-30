// SPDX-License-Identifier: GPL-3.0-or-later
//
// Delays and `$time` in the time unit of the scope that states them (IEEE 1800 section 3.14):
// each module's own `timescale`, a package task's the package's, a delay rounded to its module's
// precision, and the run counting the finest precision anything asks for. Every module states its
// time scale, and the case runs without `--timescale`, so that the unit is the one the design
// asks for.

`timescale 1ns / 1ps
package waits;
    // One nanosecond, the package's unit, whatever unit its caller counts.
    task automatic one_unit;
        #1;
    endtask
endpackage

`timescale 1ns / 1ps
module in_ns (
    input  logic       d,
    output logic       q,
    output logic [7:0] step
);
    int amount = 3;

    assign #2 q = d;

    initial begin
        step = 8'd0;
        #1.5 step = 8'd1;       // 1500 ps
        #0.0006 step = 8'd2;    // 0.6 ps, which this precision makes 1 ps: 1501 ps
        #2ps step = 8'd3;       // 1503 ps
        #amount step = 8'd4;    // 4503 ps
        $display("in_ns: $time is %0d at 4503 ps", $time);
        step <= #1 8'd5;        // 5503 ps
    end
endmodule

`timescale 10ps / 1ps
module in_tens
    import waits::*;
(
    output logic [7:0] step
);
    int amount = 4;

    initial begin
        step = 8'd0;
        #3 step = 8'd1;         // 30 ps
        #amount step = 8'd2;    // 70 ps
        one_unit();             // 1070 ps
        step = 8'd3;
        $display("in_tens: $time is %0d at 1070 ps", $time);
    end
endmodule

`timescale 1us / 1ns
module in_us (
    output logic [7:0] step
);
    initial begin
        step = 8'd0;
        #0.0036 step = 8'd1;    // 3.6 ns, which this precision makes 4 ns: 4000 ps
    end
endmodule

`timescale 1ps / 1fs
module in_ps (
    output logic [7:0] step
);
    initial begin
        step = 8'd0;
        #0.25 step = 8'd1;      // 250 fs, finer than anything else asks for
    end
endmodule

`timescale 1ps / 1fs
module tb;
    logic       d = 1'b0;
    logic       q;
    logic [7:0] ns_step;
    logic [7:0] tens_step;
    logic [7:0] us_step;
    logic [7:0] ps_step;
    int         failures = 0;

    in_ns u_ns (
        .d(d),
        .q(q),
        .step(ns_step)
    );
    in_tens u_tens (.step(tens_step));
    in_us u_us (.step(us_step));
    in_ps u_ps (.step(ps_step));

    task automatic expect_step(input logic [7:0] actual, input logic [7:0] expected,
                               input int check);
        if (actual !== expected) begin
            $display("FAIL: check %0d at %0d ps saw %0d, expected %0d", check, $time, actual,
                     expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #0.2 expect_step(ps_step, 8'd0, 1);         // 200 fs
        #0.1 expect_step(ps_step, 8'd1, 2);         // 300 fs
        $display("tb: $time is %0d at 300 fs", $time);
        #30.7 expect_step(tens_step, 8'd1, 3);      // 31 ps
        #38 expect_step(tens_step, 8'd1, 4);        // 69 ps
        #2 expect_step(tens_step, 8'd2, 5);         // 71 ps
        #29 d = 1'b1;                               // 100 ps
        #969 expect_step(tens_step, 8'd2, 6);       // 1069 ps
        #2 expect_step(tens_step, 8'd3, 7);         // 1071 ps
        #428 expect_step(ns_step, 8'd0, 8);         // 1499 ps
        #1.5 expect_step(ns_step, 8'd1, 9);         // 1500.5 ps
        #1 expect_step(ns_step, 8'd2, 10);          // 1501.5 ps
        #2 expect_step(ns_step, 8'd3, 11);          // 1503.5 ps
        #546.5 if (q !== 1'bx) begin                // 2050 ps: the change at 100 ps replaced
            $display("FAIL: q is %b at 2050 ps", q);  // the one at 0 still pending
            failures = failures + 1;
        end
        #51 if (q !== 1'b1) begin                   // 2101 ps
            $display("FAIL: q is %b at 2101 ps", q);
            failures = failures + 1;
        end
        #1898 expect_step(us_step, 8'd0, 12);       // 3999 ps
        $display("tb: $time is %0d at 3999 ps, $stime %0d", $time, $stime);
        #2 expect_step(us_step, 8'd1, 13);          // 4001 ps
        #501 expect_step(ns_step, 8'd3, 14);        // 4502 ps
        #2 expect_step(ns_step, 8'd4, 15);          // 4504 ps
        #998 expect_step(ns_step, 8'd4, 16);        // 5502 ps
        #2 expect_step(ns_step, 8'd5, 17);          // 5504 ps

        if (failures == 0) $display("PASS timescales");
        $finish;
    end
endmodule
