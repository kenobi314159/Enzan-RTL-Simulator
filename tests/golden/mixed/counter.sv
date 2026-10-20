// SPDX-License-Identifier: GPL-3.0-or-later
//
// A counter a VHDL testbench instantiates, whose width a generic sets through a parameter with no
// default, and whose limit a generic sets or leaves as it is.
module counter #(
    parameter int W,
    parameter int LIMIT = 0
) (
    input logic clk,
    input logic rst,
    input logic en,
    output logic [W-1:0] q,
    output logic wrapped
);
    localparam logic [W-1:0] LAST = W'(LIMIT != 0 ? LIMIT - 1 : 0);

    always_ff @(posedge clk) begin
        if (rst) begin
            q <= '0;
            wrapped <= 1'b0;
        end else if (en) begin
            q <= LIMIT != 0 && q == LAST ? '0 : q + 1'b1;
            wrapped <= LIMIT != 0 ? q == LAST : &q;
        end
    end
endmodule
