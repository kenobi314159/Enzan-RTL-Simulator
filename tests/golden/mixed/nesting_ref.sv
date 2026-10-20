// SPDX-License-Identifier: GPL-3.0-or-later
//
// nesting.vhd in SystemVerilog, which iverilog runs nesting.sv against.
`timescale 1ns / 1ps

module Delay #(
    parameter int Width = 2
) (
    input logic Clk,
    input logic [Width-1:0] D,
    output logic [Width-1:0] Q
);
    logic [Width-1:0] between;
    Stage #(.N(Width)) first (.clk(Clk), .d(D), .q(between));
    Stage #(.N(Width)) second (.clk(Clk), .d(between), .q(Q));
endmodule
