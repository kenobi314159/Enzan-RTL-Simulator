// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scalar real ports (IEEE 1800 section 23): an input connected directly, one connected to an
// expression that needs an intermediate object, and an output written procedurally. A wrapper
// passes both directions through another pair of real ports.

`timescale 1ns / 1ps

module scale(input real value, input real factor, output real result);
    always_comb result = value * factor;
endmodule

module wrapper(input real value, output real result);
    scale inner(.value(value), .factor(2.5), .result(result));
endmodule

module tb;
    real source = 1.2;
    real result;
    int failures = 0;

    wrapper dut(.value(source), .result(result));

    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures++;
        end
    endtask

    initial begin
        #1;
        $display("result=%0.2f", result);
        check(1, result == 3.0);

        source = -2.0;
        #1;
        $display("result=%0.2f", result);
        check(2, result == -5.0);

        if (failures == 0) $display("PASS real_ports");
        $finish;
    end
endmodule
