// SPDX-License-Identifier: GPL-3.0-or-later
//
// A second top level that cells reach by name, as AMD's simulation models reach `glbl`: its reset
// is held for the first 100 units, each flip-flop reads it from the depth it is at, and each cell
// drives a done flag beside it. A reference into another top level is a path from the root, the
// same from every depth, so the two depths below share a definition and both see the reset.

module glbl;
    reg GSR_int;
    wire GSR;
    wire DONE;
    assign GSR = GSR_int;
    initial begin
        GSR_int = 1'b1;
        #100 GSR_int = 1'b0;
    end
endmodule

module flop #(parameter logic INIT = 1'b1) (input logic clk, input logic d, output logic q);
    tri0 gsr = glbl.GSR;
    always @(posedge clk or posedge gsr)
        if (gsr) q <= INIT;
        else q <= d;
    // Every flop drives the same flag, so it is a net with a driver per cell.
    assign glbl.DONE = !gsr;
endmodule

module pair (input logic clk, input logic d, output logic q);
    flop first (.clk(clk), .d(d), .q(q));
endmodule

module tb;
    logic clk = 1'b0;
    logic d = 1'b0;
    logic shallow, deep;
    int failures = 0;

    flop u_flop (.clk(clk), .d(d), .q(shallow));
    pair u_pair (.clk(clk), .d(d), .q(deep));

    always #5 clk = ~clk;

    initial begin
        #50;
        $display("t=%0d gsr=%b shallow=%b deep=%b done=%b", $time, glbl.GSR, shallow, deep,
                 glbl.DONE);
        if (shallow !== 1'b1 || deep !== 1'b1 || glbl.DONE !== 1'b0) begin
            $display("FAIL: the reset should hold both flops at INIT");
            failures = failures + 1;
        end
        #100;
        $display("t=%0d gsr=%b shallow=%b deep=%b done=%b", $time, glbl.GSR, shallow, deep,
                 glbl.DONE);
        if (shallow !== 1'b0 || deep !== 1'b0 || glbl.DONE !== 1'b1) begin
            $display("FAIL: released, both flops should follow d");
            failures = failures + 1;
        end
        if (failures == 0) $display("PASS glbl");
        $finish;
    end
endmodule
