// SPDX-License-Identifier: GPL-3.0-or-later
//
// A nonblocking event trigger, `->>` (IEEE 1800 section 15.5.1), wakes what waits on the event in
// the NBA region, after the statements of the time step that follow it. Icarus Verilog does not
// support it, so the case checks itself.

module tb;
    event late;
    logic seen = 1'b0, early = 1'b0;
    initial begin
        @late;
        seen = 1'b1;
    end

    initial begin
        #1 ->> late;
        early = seen;
        #1 $display("early=%b seen=%b", early, seen);
        if (!early && seen) $display("PASS nonblocking_events");
        $finish;
    end
endmodule
