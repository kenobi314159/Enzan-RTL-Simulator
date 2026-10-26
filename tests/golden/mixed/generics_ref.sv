// SPDX-License-Identifier: GPL-3.0-or-later
//
// generics.vhd in SystemVerilog, which iverilog runs generics.sv against.
`timescale 1ns / 1ps

module Counter #(
    parameter int Width = 4,
    parameter int Step = 1,
    parameter bit Enabled = 1
) (
    input logic Clk,
    input logic Rst,
    output logic [Width-1:0] Count,
    output int Total,
    output bit Wrapped
);
    always_ff @(posedge Clk) begin
        if (Rst) begin
            Count <= '0;
            Total <= 0;
            Wrapped <= 1'b0;
        end else begin
            Total <= Total + 1;
            if (Enabled) begin
                Count <= Count + Width'(Step);
                Wrapped <= Width'(Count + Width'(Step)) < Count;
            end
        end
    end
endmodule
