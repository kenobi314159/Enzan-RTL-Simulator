// SPDX-License-Identifier: GPL-3.0-or-later
//
// The design the cocotb golden test drives. It has no delays of its own: every event comes from
// the testbench, which is what a cocotb testbench is.

module counter (
    input  logic       clk,
    input  logic       rst,
    input  logic       enable,
    output logic [7:0] count
);
    always_ff @(posedge clk)
        if (rst)
            count <= 8'd0;
        else if (enable)
            count <= count + 8'd1;
endmodule
