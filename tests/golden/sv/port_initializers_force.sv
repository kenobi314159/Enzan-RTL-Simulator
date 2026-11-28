// SPDX-License-Identifier: GPL-3.0-or-later
//
// An output variable drives the external net through a continuous assignment
// (IEEE 1800-2017 section 6.5), so forcing that net cannot write the variable.
// Icarus collapses a lone output driver and propagates the force backwards.

module initialized(output logic [7:0] state = 8'ha5);
endmodule

module tb;
    wire [7:0] state;
    initialized child(state);
    initial begin
        #1;
        if (state !== 8'ha5 || child.state !== 8'ha5) $fatal(1, "initial value");
        force state = 8'h12;
        #1;
        if (state !== 8'h12 || child.state !== 8'ha5) $fatal(1, "force propagated backwards");
        release state;
        #1;
        if (state !== 8'ha5 || child.state !== 8'ha5) $fatal(1, "release did not restore driver");
        $display("PASS port_initializers_force");
        $finish;
    end
endmodule
