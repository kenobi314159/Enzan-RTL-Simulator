// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module whose parameters decide only values is one generic unit, specialised at elaboration
// for each set of values its instances give it (DESIGN_GUIDE.md section 18, M5.1). Each instance
// must see its own values: one that sets nothing gets the declared defaults rather than the
// values of whichever instance the unit was translated from, a local parameter follows the
// parameters it is defined from, and a parameter passed down from a generic parent is the
// parent's own.

module wrap_counter #(parameter int MAX = 9, parameter int STEP = 1) (
    input  logic       clk,
    input  logic       rst,
    output logic [7:0] count
);
    localparam int LAST = MAX - STEP;
    always_ff @(posedge clk) begin
        if (rst) count <= '0;
        else if (count > 8'(LAST)) count <= '0;
        else count <= count + 8'(STEP);
    end
endmodule

// Passes its own parameter down, doubled, and branches on it in a process.
module pair #(parameter int LIMIT = 3) (
    input  logic       clk,
    input  logic       rst,
    output logic [7:0] slow,
    output logic [7:0] fast,
    output logic       above
);
    wrap_counter #(.MAX(LIMIT)) u_slow (.clk, .rst, .count(slow));
    wrap_counter #(.MAX(LIMIT * 2), .STEP(2)) u_fast (.clk, .rst, .count(fast));
    always_comb begin
        if (LIMIT > 4) above = 1'b1;
        else above = 1'b0;
    end
endmodule

module tb;
    logic clk = 1'b0;
    logic rst = 1'b1;
    logic [7:0] a, b, c, lo_slow, lo_fast, hi_slow, hi_fast;
    logic lo_above, hi_above;
    int failures = 0;

    wrap_counter #(.MAX(5)) u_a (.clk, .rst, .count(a));
    wrap_counter #(.MAX(20), .STEP(3)) u_b (.clk, .rst, .count(b));
    wrap_counter u_c (.clk, .rst, .count(c));
    pair u_lo (.clk, .rst, .slow(lo_slow), .fast(lo_fast), .above(lo_above));
    pair #(.LIMIT(7)) u_hi (.clk, .rst, .slow(hi_slow), .fast(hi_fast), .above(hi_above));

    always #5 clk = ~clk;

    task automatic check(int which, logic [7:0] got, logic [7:0] expected);
        if (got !== expected) begin
            $display("FAIL: check %0d is %0d, expected %0d", which, got, expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #12 rst = 1'b0;
        #200;
        $display("a=%0d b=%0d c=%0d lo=%0d,%0d,%0d hi=%0d,%0d,%0d", a, b, c, lo_slow,
                 lo_fast, lo_above, hi_slow, hi_fast, hi_above);
        // Twenty rising edges after reset; a counter wraps after (MAX - STEP) / STEP + 2 values.
        check(1, a, 8'd2);
        check(2, b, 8'd18);
        check(3, c, 8'd0);  // u_c sets nothing: the declared defaults
        check(4, lo_slow, 8'd0);
        check(5, lo_fast, 8'd0);
        check(6, {7'd0, lo_above}, 8'd0);
        check(7, hi_slow, 8'd4);
        check(8, hi_fast, 8'd8);
        check(9, {7'd0, hi_above}, 8'd1);
        if (failures != 0) $fatal(1, "FAIL generic_parameters: %0d checks failed", failures);
        $display("PASS generic_parameters");
        $finish;
    end
endmodule
