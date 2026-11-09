// SPDX-License-Identifier: GPL-3.0-or-later
//
// A parameter in a module path's condition, which IEEE 1800 section 30.4.4 does not allow, only
// specparams being among its constants, and which models written for other simulators rely on:
// it is accepted as a warning, and chooses each instance's path by its parameter, as Icarus
// Verilog has it.

`timescale 1ns / 1ns

module chooser #(parameter INVERTED = 1'b0) (input a, input en, output y);
    assign y = a;
    specify
        if (!INVERTED) (a => y) = 5;
        if (INVERTED && en) (a => y) = 9;
    endspecify
endmodule

module tb;
    // `a` stays x until 10, so that nothing changes at time 0, where Icarus chooses a path as if
    // `en` were not yet set.
    logic a, en = 1'b1;
    wire y_plain, y_inverted;
    chooser #(.INVERTED(1'b0)) u_plain (.a(a), .en(en), .y(y_plain));
    chooser #(.INVERTED(1'b1)) u_inverted (.a(a), .en(en), .y(y_inverted));

    always @(y_plain) $display("%0d y_plain=%b", $time, y_plain);
    always @(y_inverted) $display("%0d y_inverted=%b", $time, y_inverted);

    initial begin
        #10 a = 1'b1;              // 5 on in the plain instance, 9 in the inverted one
        #20 en = 1'b0;
        #1 a = 1'b0;               // the inverted one's path is off: at once
        #20 $display("PASS parameter_path_conditions");
        $finish;
    end
endmodule
