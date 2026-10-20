// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module with a port of each kind a VHDL component's port is converted to: std_logic and its
// vectors each way, integers, a bit and a boolean as two-state bits, and a bus both sides drive and
// release.
module ports (
    input logic sel,
    input logic [7:0] a,
    input logic [7:0] b,
    output logic [7:0] y,
    input int n,
    output int doubled,
    input bit flag,
    output bit inverted,
    input logic drive,
    inout wire [3:0] data,
    output logic [3:0] seen
);
    assign y = sel ? b : a;
    assign doubled = n * 2;
    assign inverted = !flag;
    assign data = drive ? 4'b1010 : 4'bz;
    assign seen = data;
endmodule
