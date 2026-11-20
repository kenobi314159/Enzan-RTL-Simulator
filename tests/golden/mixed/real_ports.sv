// SPDX-License-Identifier: GPL-3.0-or-later
//
// A SystemVerilog testbench around the VHDL real_stage (real_ports.vhd).  Its real crosses into
// VHDL, then through a SystemVerilog component, before VHDL returns it to the testbench.

`timescale 1ns / 1ps

module RealGain(input real value, output real result);
    assign result = value * 1.5;
endmodule

module tb;
    real value = 2.0;
    real result;
    int failures = 0;

    real_stage dut(.value(value), .result(result));

    task automatic check(input int which, input real expected);
        if (result != expected) begin
            $display("FAIL: check %0d: got %0.2f, expected %0.2f", which, result, expected);
            failures++;
        end
    endtask

    initial begin
        #1;
        $display("result=%0.2f", result);
        check(1, 5.0);

        value = -4.0;
        #1;
        $display("result=%0.2f", result);
        check(2, -4.0);

        value = 0.5;
        #1;
        $display("result=%0.2f", result);
        check(3, 2.75);

        if (failures == 0) $display("PASS real_ports");
        $finish;
    end
endmodule
