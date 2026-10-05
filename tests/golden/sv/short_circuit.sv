// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operands the language may leave unevaluated (IEEE 1800 sections 11.4.7 and 11.4.11): the
// right operand of `&&` when the left is false and of `||` when it is true, and the arm of `?:`
// its condition does not select. Their side effects must not happen. An unknown left operand
// or condition decides nothing, so then everything is evaluated.

module tb;
    int unsigned count;
    int unsigned left_arm;
    int unsigned right_arm;
    logic        unknown;
    logic        result;
    int unsigned selected;
    int failures = 0;

    function automatic bit bump();
        count++;
        return 1'b1;
    endfunction

    initial begin
        count = 0;
        if (1'b0 && ++count > 0) $display("FAIL: 0 && ... is false");
        if (1'b1 || ++count > 0) result = 1'b1;
        result = 1'b0 && bump();
        result = 1'b1 || bump();
        if (count != 0) begin
            $display("FAIL: a decided && or || evaluated its right operand, count=%0d", count);
            failures++;
        end

        result = 1'b1 && bump();
        result = 1'b0 || bump();
        if (count != 2 || result !== 1'b1) begin
            $display("FAIL: an undecided && or || skipped its right operand, count=%0d", count);
            failures++;
        end

        // An unknown left operand: the right is evaluated, and decides where it can.
        unknown = 1'bx;
        result = unknown && ++count == 3;
        if (count != 3 || result !== 1'bx) begin
            $display("FAIL: x && 1 gave %b with count=%0d", result, count);
            failures++;
        end
        result = unknown || bump();
        if (count != 4 || result !== 1'b1) begin
            $display("FAIL: x || 1 gave %b with count=%0d", result, count);
            failures++;
        end

        left_arm = 0;
        right_arm = 0;
        selected = 1'b1 ? ++left_arm : ++right_arm;
        selected = 1'b0 ? ++left_arm : ++right_arm;
        if (left_arm != 1 || right_arm != 1 || selected != 1) begin
            $display("FAIL: ?: evaluated the arm it did not select, %0d and %0d", left_arm,
                     right_arm);
            failures++;
        end

        // An unknown condition evaluates both arms and merges them bit by bit.
        selected = unknown ? ++left_arm : ++right_arm;
        if (left_arm != 2 || right_arm != 2 || selected !== 32'd2) begin
            $display("FAIL: x ? ++a : ++b gave %0d with %0d and %0d", selected, left_arm,
                     right_arm);
            failures++;
        end

        if (failures != 0) begin
            $display("FAIL short_circuit: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS short_circuit");
        $finish;
    end
endmodule
