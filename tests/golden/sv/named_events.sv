// SPDX-License-Identifier: GPL-3.0-or-later
//
// Named events (IEEE 1800 section 15.5), as AMD's newer clock managers pace their clocks by them:
// `->` wakes what waits on the event at once, and a process that waits again sees the next
// trigger. nonblocking_events.sv has `->>`, which Icarus Verilog does not support.

module tb;
    event rose, fell;
    logic clk = 1'b0;
    always #5 clk = ~clk;
    always @(posedge clk) -> rose;
    always @(negedge clk) -> fell;

    int rises = 0, falls = 0;
    initial forever begin
        @rose;
        rises = rises + 1;
    end
    always @fell falls = falls + 1;

    initial begin
        #42;
        $display("t=%0d rises=%0d falls=%0d", $time, rises, falls);
        if (rises == 4 && falls == 4) $display("PASS named_events");
        $finish;
    end
endmodule
