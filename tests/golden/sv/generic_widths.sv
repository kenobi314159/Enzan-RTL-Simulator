// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module whose parameter decides its widths is one generic unit, specialised at elaboration for
// each width its instances ask for (DESIGN_GUIDE.md section 18, M5.1). Every width an expression
// takes from its context moves with the parameter: an operand narrower than the unit's own
// registers for one width is wider for another, and must be extended, or truncated, accordingly.

module accumulator #(parameter int W = 8) (
    input  logic         clk,
    input  logic         rst,
    input  logic [W-1:0] step,
    output logic [W-1:0] total,
    output logic [W:0]   sum,
    output logic         above
);
    // `sum` is one bit wider than the operands, so the addition keeps its carry in every
    // specialisation; `8'd100` is wider than `total` for W = 4 and narrower for W = 12.
    always_comb sum = total + step;
    always_comb above = total > 8'd100;
    always_ff @(posedge clk) begin
        if (rst) total <= '0;
        else total <= total + step;
    end
endmodule

module tb;
    logic clk = 1'b0;
    logic rst = 1'b1;
    logic [3:0] a;
    logic [11:0] b;
    logic [7:0] c;
    logic [4:0] a_sum;
    logic [12:0] b_sum;
    logic [8:0] c_sum;
    logic a_above, b_above, c_above;
    int failures = 0;

    accumulator #(.W(4)) u_a (.clk, .rst, .step(4'd11), .total(a), .sum(a_sum), .above(a_above));
    accumulator #(.W(12)) u_b (.clk, .rst, .step(12'd700), .total(b), .sum(b_sum),
                               .above(b_above));
    accumulator u_c (.clk, .rst, .step(8'd77), .total(c), .sum(c_sum), .above(c_above));

    always #5 clk = ~clk;

    task automatic check(int which, logic [12:0] got, logic [12:0] expected);
        if (got !== expected) begin
            $display("FAIL: check %0d is %0d, expected %0d", which, got, expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #12 rst = 1'b0;
        #200;
        $display("a=%0d,%0d,%0d b=%0d,%0d,%0d c=%0d,%0d,%0d", a, a_sum, a_above, b, b_sum,
                 b_above, c, c_sum, c_above);
        // Twenty rising edges after reset: the total is 20 steps, wrapped at the unit's width.
        check(1, a, 13'(20 * 11 % 16));
        check(2, a_sum, 13'(20 * 11 % 16 + 11));
        check(3, a_above, 13'd0);
        check(4, b, 13'(20 * 700 % 4096));
        check(5, b_sum, 13'(20 * 700 % 4096 + 700));
        check(6, b_above, 13'd1);
        check(7, c, 13'(20 * 77 % 256));
        check(8, c_sum, 13'(20 * 77 % 256 + 77));
        check(9, c_above, 13'd0);
        if (failures != 0) $fatal(1, "FAIL generic_widths: %0d checks failed", failures);
        $display("PASS generic_widths");
        $finish;
    end
endmodule
