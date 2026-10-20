// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two registers in a row: a module a VHDL testbench instantiates, holding two instances of another.
module stage #(
    parameter int W = 4
) (
    input logic clk,
    input logic [W-1:0] d,
    output logic [W-1:0] q
);
    always_ff @(posedge clk) q <= d;
endmodule

module pipeline #(
    parameter int W = 4
) (
    input logic clk,
    input logic [W-1:0] d,
    output logic [W-1:0] q,
    output logic [W-1:0] middle
);
    stage #(.W(W)) first (.clk, .d, .q(middle));
    stage #(.W(W)) second (.clk, .d(middle), .q);
endmodule
