// SPDX-License-Identifier: GPL-3.0-or-later
//
// A counter that wraps at a limit a test may change, in SystemVerilog.
module sv_counter #(
    parameter int WIDTH = 8
) (
    input logic clk,
    input logic rst,
    output logic [WIDTH-1:0] count,
    output logic wrapped
);
    logic [WIDTH-1:0] limit = '1;

    always_ff @(posedge clk) begin
        if (rst) count <= '0;
        else count <= count == limit ? '0 : count + 1'b1;
    end
    assign wrapped = !rst && count == limit;
endmodule
