// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's carry chains CARRY4 and CARRY8 as the adders synthesis makes of them, S = a ^ b and
// DI = a: the sum on O and the carries on CO, over every pair of 4-bit values and a spread of
// 8-bit ones, with a carry in.

`timescale 1ns / 1ps

module tb_carry;
    logic [3:0] a4, b4;
    logic ci4;
    wire [3:0] o4, co4;
    CARRY4 u4 (.CO(co4), .O(o4), .CI(ci4), .CYINIT(1'b0), .DI(a4), .S(a4 ^ b4));

    logic [7:0] a8, b8;
    logic ci8;
    wire [7:0] o8, co8;
    CARRY8 #(.CARRY_TYPE("SINGLE_CY8")) u8 (.CO(co8), .O(o8), .CI(ci8), .CI_TOP(1'b0), .DI(a8),
                                           .S(a8 ^ b8));

    int failures = 0;
    initial begin
        for (int carry = 0; carry < 2; carry++)
            for (int x = 0; x < 16; x++)
                for (int y = 0; y < 16; y++) begin
                    {a4, b4, ci4} = {x[3:0], y[3:0], carry[0]};
                    #1;
                    if ({co4[3], o4} !== 5'(a4 + b4 + ci4)) begin
                        $display("FAIL: %0d + %0d + %0d gave %b %b", a4, b4, ci4, co4, o4);
                        failures = failures + 1;
                    end
                end
        $display("CARRY4 checked");

        for (int step = 0; step < 512; step++) begin
            {a8, b8, ci8} = {8'(step * 37), 8'(step * 101 + 13), step[0]};
            #1;
            if ({co8[7], o8} !== 9'(a8 + b8 + ci8)) begin
                $display("FAIL: %0d + %0d + %0d gave %b %b", a8, b8, ci8, co8, o8);
                failures = failures + 1;
            end
        end
        $display("CARRY8 checked");

        if (failures == 0) $display("PASS tb_carry");
        $finish;
    end
endmodule
