// SPDX-License-Identifier: GPL-3.0-or-later
//
// real_ports.vhd in SystemVerilog, for Icarus Verilog's reference run.

module real_stage(input real value, output real result);
    assign result = (value * 1.5) + 2.0;
endmodule
