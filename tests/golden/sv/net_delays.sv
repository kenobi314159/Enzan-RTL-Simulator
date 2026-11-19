// SPDX-License-Identifier: GPL-3.0-or-later
//
// Net declaration delays after resolution, and a delay on a declaration assignment belonging to
// that assignment alone (IEEE 1800 section 10.3.3).

`timescale 1ns / 1ns

module tb;
    logic high = 1'b0, low = 1'b0;
    tri #5 resolved;
    assign resolved = high ? 1'b1 : 1'bz;
    assign resolved = low ? 1'b0 : 1'bz;

    logic transition_driver = 1'b0;
    wire #(2, 5, 3) transition_delayed;
    assign transition_delayed = transition_driver;

    logic declaration_driver = 1'b0, immediate_driver = 1'bz;
    wire #5 declaration_delayed = declaration_driver;
    assign declaration_delayed = immediate_driver;

    initial begin
        #1 if (resolved !== 1'bz) $fatal(1, "net did not begin undriven: %b", resolved);
        #9 high = 1'b1;
        #4 if (resolved !== 1'bz) $fatal(1, "early resolved one");
        #2 if (resolved !== 1'b1) $fatal(1, "late resolved one");
        #4 low = 1'b1;
        #4 if (resolved !== 1'b1) $fatal(1, "early resolved contention");
        #2 if (resolved !== 1'bx) $fatal(1, "late resolved contention");
        #4 high = 1'b0;
        #4 if (resolved !== 1'bx) $fatal(1, "early resolved zero");
        #2 if (resolved !== 1'b0) $fatal(1, "late resolved zero");
        #4 low = 1'b0;
        #4 if (resolved !== 1'b0) $fatal(1, "early resolved high impedance");
        #2 if (resolved !== 1'bz) $fatal(1, "late resolved high impedance");
    end

    initial begin
        #6 if (transition_delayed !== 1'b0) $fatal(1, "initial fall delay");
        #4 transition_driver = 1'b1;
        #1 if (transition_delayed !== 1'b0) $fatal(1, "early rise");
        #2 if (transition_delayed !== 1'b1) $fatal(1, "late rise");
        #7 transition_driver = 1'b0;
        #4 if (transition_delayed !== 1'b1) $fatal(1, "early fall");
        #2 if (transition_delayed !== 1'b0) $fatal(1, "late fall");
        #4 transition_driver = 1'bz;
        #2 if (transition_delayed !== 1'b0) $fatal(1, "early turn-off");
        #2 if (transition_delayed !== 1'bz) $fatal(1, "late turn-off");
    end

    initial begin
        #6 if (declaration_delayed !== 1'b0) $fatal(1, "declaration assignment delay");
        #4 immediate_driver = 1'b1;
        #1 if (declaration_delayed !== 1'bx) $fatal(1, "other driver was delayed");
        #9 immediate_driver = 1'bz;
        #1 if (declaration_delayed !== 1'b0) $fatal(1, "other driver release was delayed");
        #9 declaration_driver = 1'b1;
        #4 if (declaration_delayed !== 1'b0) $fatal(1, "early declaration assignment");
        #2 if (declaration_delayed !== 1'b1) $fatal(1, "late declaration assignment");
    end

    initial begin
        #60 $display("PASS net_delays");
        $finish;
    end
endmodule
