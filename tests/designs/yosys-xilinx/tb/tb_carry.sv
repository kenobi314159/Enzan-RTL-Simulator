// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's carry chains CARRY4 and CARRY8, the second as one chain of eight and as two of four,
// against a chain of the testbench's own, over inputs that walk through every carry and select
// pattern. CARRY4's module paths differ from bit to bit of its outputs, each from its own inputs;
// every check is of a settled value, a nanosecond after the inputs change, its longest path being
// 642 ps.
//
// Icarus: without -gspecify, since it applies a path to a bit of CARRY4's outputs to all of them.

`timescale 1ns / 1ps

module tb_carry;
    logic ci = 1'b0, cyinit = 1'b0, ci_top = 1'b0;
    logic [7:0] di = 8'h00, s = 8'h00;
    wire [3:0] co4, o4;
    wire [7:0] co8, o8, co_dual, o_dual;
    CARRY4 u4 (.CO(co4), .O(o4), .CI(ci), .CYINIT(cyinit), .DI(di[3:0]), .S(s[3:0]));
    CARRY8 u8 (.CO(co8), .O(o8), .CI(ci), .CI_TOP(ci_top), .DI(di), .S(s));
    CARRY8 #(.CARRY_TYPE("DUAL_CY4")) u_dual (
        .CO(co_dual), .O(o_dual), .CI(ci), .CI_TOP(ci_top), .DI(di), .S(s));

    // A chain of `width` bits from `carry_in`, from `low` up: each bit propagates the carry when
    // its select is high and takes its DI otherwise, and its sum is the select and the carry in.
    function automatic logic [15:0] chain(logic [7:0] sel, logic [7:0] data, logic carry_in,
                                          int low, int width);
        logic [7:0] carries = 8'h00, sums = 8'h00;
        logic carry = carry_in;
        for (int bit_index = low; bit_index < low + width; bit_index = bit_index + 1) begin
            sums[bit_index] = sel[bit_index] ^ carry;
            carry = sel[bit_index] ? carry : data[bit_index];
            carries[bit_index] = carry;
        end
        return {carries, sums};
    endfunction

    int failures = 0;
    task automatic compare(int which);
        logic [15:0] want4, want8, want_dual, low_half, high_half;
        want4 = chain(s, di, ci | cyinit, 0, 4);
        want8 = chain(s, di, ci, 0, 8);
        low_half = chain(s, di, ci, 0, 4);
        high_half = chain(s, di, ci_top, 4, 4);
        want_dual = low_half | high_half;
        if ({co4, o4} !== {want4[11:8], want4[3:0]} || {co8, o8} !== want8
            || {co_dual, o_dual} !== want_dual) begin
            $display("FAIL: check %0d with s=%h di=%h ci=%b cyinit=%b ci_top=%b", which, s, di, ci,
                     cyinit, ci_top);
            failures = failures + 1;
        end
    endtask

    initial begin
        for (int n = 0; n < 512; n = n + 1) begin
            {ci_top, cyinit, ci} = 3'(n);
            s = 8'(n * 29 + n / 7);
            di = 8'(n * 83 ^ n / 3);
            #1 compare(n);
        end
        if (failures == 0) $display("PASS tb_carry");
        $finish;
    end
endmodule
