// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real math system functions of IEEE 1800 section 20.8.2. Their operands are variables so
// slang cannot fold the calls and each one reaches Enzan's runtime.

module tb;
    real zero = 0.0;
    real one = 1.0;
    real two = 2.0;
    real three = 3.0;
    real four = 4.0;
    real five = 5.0;
    real thousand = 1000.0;
    real positive = 2.75;
    real negative = -2.75;
    int failures = 0;

    function automatic bit near(real actual, real expected, real tolerance);
        real difference;
        difference = actual - expected;
        if (difference < 0.0) difference = -difference;
        return difference <= tolerance;
    endfunction

    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures++;
        end
    endtask

    initial begin
        check(1, near($ln($exp(one)), one, 1.0e-12));
        check(2, near($log10(thousand), three, 1.0e-12));
        check(3, $sqrt(thousand - 919.0) == 9.0);
        check(4, $floor(positive) == two && $ceil(positive) == three);
        check(5, $floor(negative) == -3.0 && $ceil(negative) == -2.0);

        check(6, $sin(zero) == zero && $cos(zero) == one && $tan(zero) == zero);
        check(7, $asin(zero) == zero && $acos(one) == zero && $atan(zero) == zero);
        check(8, $sinh(zero) == zero && $cosh(zero) == one && $tanh(zero) == zero);
        check(9, $asinh(zero) == zero && $acosh(one) == zero && $atanh(zero) == zero);

        check(10, $pow(two, five) == 32.0);
        check(11, $atan2(zero, one) == zero);
        check(12, $hypot(three, four) == five);

        if (failures == 0) $display("PASS real_math");
        $finish;
    end
endmodule
