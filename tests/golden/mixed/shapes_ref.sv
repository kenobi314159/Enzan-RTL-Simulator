// SPDX-License-Identifier: GPL-3.0-or-later
//
// shapes.vhd in SystemVerilog, which iverilog runs shapes.sv against: a record is a packed struct,
// its first field highest, and an array of vectors an unpacked array of them.
`timescale 1ns / 1ps

module Shapes (
    input logic [12:0] Inp,
    output logic [12:0] Outp,
    input logic [7:0] Lane [0:3],
    output logic [9:0] Sum,
    input logic Drive,
    inout wire [3:0] Data,
    output logic [3:0] Seen
);
    assign Outp = {Inp[4:1], Inp[8:5], Inp[12:9], ~Inp[0]};
    assign Sum = 10'(Lane[0]) + 10'(Lane[1]) + 10'(Lane[2]) + 10'(Lane[3]);
    assign Data = Drive ? 4'b1010 : 4'bz;
    assign Seen = Data;
endmodule
