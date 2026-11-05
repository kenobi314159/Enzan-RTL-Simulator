// SPDX-License-Identifier: GPL-3.0-or-later
//
// A module whose parameters are declared as vendors' simulation models declare theirs, which a
// VHDL testbench (parameters.vhd) sets from generics: a boolean as a string, "TRUE" or "FALSE",
// a flag as one bit, a mode as a string and a count as an integer.
module flags #(
    parameter ENABLED = "TRUE",
    parameter [0:0] INVERTED = 1'b0,
    parameter MODE = "SYNC",
    parameter integer COUNT = 0
) (
    output logic is_enabled,
    output logic is_valid,
    output logic is_inverted,
    output logic is_sync,
    output logic [7:0] value
);
    assign is_enabled = ENABLED == "TRUE";
    assign is_valid = ENABLED == "TRUE" || ENABLED == "FALSE";
    assign is_inverted = INVERTED;
    assign is_sync = MODE == "SYNC";
    assign value = COUNT;
endmodule
