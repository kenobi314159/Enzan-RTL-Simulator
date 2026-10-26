// SPDX-License-Identifier: GPL-3.0-or-later
//
// A SystemVerilog toplevel holding a counter of each language, for cocotb to reach through VPI and,
// for the VHDL one, through VHPI.
module sv_top (
    input logic clk,
    input logic rst
);
    logic [7:0] sv_count;
    logic [3:0] vhdl_count;
    logic sv_wrapped, vhdl_wrapped;

    sv_counter #(.WIDTH(8)) u_sv (.clk, .rst, .count(sv_count), .wrapped(sv_wrapped));
    vhdl_counter #(.WIDTH(4)) u_vhdl (.clk, .rst, .count(vhdl_count), .wrapped(vhdl_wrapped));
endmodule
