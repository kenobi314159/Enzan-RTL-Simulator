// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module path delays (IEEE 1800 section 30.4), as AMD's models give their clock-to-output
// delays: an output changes after the delay of the path whose input changed most recently, the
// shortest of those that changed together, and no earlier than the logic behind it changes.
//
// - `mix` has two paths to one output, the second conditional, and an input with none; a reader
//   inside it sees the delayed output.
// - `edges` has rise and fall delays, which the transitions to and from x and z derive from.
// - `flop` has an edge-sensitive path from its clock; its reset has none, and a clock edge the
//   path does not name does not count.
// - `recent` shows the input that changed last deciding, though another path ends later.
// - `word` delays a vector by a full path, and `wrapper` an output an instance inside drives.
// A pulse shorter than its path's delay never reaches the output.

`timescale 1ns / 1ns

module mix (input a, input b, input c, input en, output y, output seen);
    assign y = a ^ b ^ c;
    assign seen = y;
    specify
        (a => y) = 5;
        if (en) (b => y) = 10;
    endspecify
endmodule

module edges (input a, output y);
    assign y = a;
    specify
        (a => y) = (3, 7);
    endspecify
endmodule

module flop (input clk, input d, input rst, output reg q);
    always @(posedge clk or posedge rst)
        if (rst) q <= 1'b0;
        else q <= d;
    specify
        (posedge clk => (q +: d)) = 7;
    endspecify
endmodule

module recent (input a, input b, output y);
    assign #5 y = a;
    specify
        (a => y) = 10;
        (b => y) = 20;
    endspecify
endmodule

module word (input clk, input [3:0] d, output reg [3:0] q);
    always @(posedge clk) q <= d;
    specify
        (clk *> q) = 4;
    endspecify
endmodule

module wrapper (input clk, input [3:0] d, output [3:0] q);
    word inner (.clk(clk), .d(d), .q(q));
    specify
        (clk *> q) = 2;
    endspecify
endmodule

module tb;
    logic a = 1'b0, b = 1'b0, c = 1'b0, en = 1'b1;
    wire y, seen;
    mix u_mix (.a(a), .b(b), .c(c), .en(en), .y(y), .seen(seen));

    logic e = 1'b0;
    wire ey;
    edges u_edges (.a(e), .y(ey));

    logic clk = 1'b0, d = 1'b0, rst = 1'b0;
    wire q;
    flop u_flop (.clk(clk), .d(d), .rst(rst), .q(q));

    logic ra = 1'b0, rb = 1'b0;
    wire ry;
    recent u_recent (.a(ra), .b(rb), .y(ry));

    logic wclk = 1'b0;
    logic [3:0] wd = 4'h0;
    wire [3:0] wq, wrapped;
    word u_word (.clk(wclk), .d(wd), .q(wq));
    wrapper u_wrapper (.clk(wclk), .d(wd), .q(wrapped));

    always @(y) $display("%0d y=%b", $time, y);
    always @(seen) $display("%0d seen=%b", $time, seen);
    always @(ey) $display("%0d ey=%b", $time, ey);
    always @(q) $display("%0d q=%b", $time, q);
    always @(ry) $display("%0d ry=%b", $time, ry);
    always @(wq) $display("%0d wq=%h", $time, wq);
    always @(wrapped) $display("%0d wrapped=%h", $time, wrapped);

    initial begin
        #20 a = 1'b1;              // a's path: 25
        #20 b = 1'b1;              // b's, the more recent: 50
        #20 c = 1'b1;              // no path, and both have run out: at once
        #20 en = 1'b0;
        #1 b = 1'b0;               // b's path is off, a's has run out: at once
        #20 en = 1'b1;

        #20 e = 1'b1;              // rise, 3
        #20 e = 1'b0;              // fall, 7
        #20 e = 1'bx;              // 0 to x, the shorter, 3
        #20 e = 1'b0;              // x to 0, the longer, 7
        #20 e = 1'bz;              // 0 to z, as a rise
        #20 e = 1'b1;              // z to 1, as a rise

        #20 a = 1'b0;
        #2 a = 1'b1;               // a pulse of 2 against a delay of 5, which never arrives

        #20 d = 1'b1;
        #5 clk = 1'b1;             // q rises 7 after the edge
        #10 clk = 1'b0;            // a falling edge, which the path does not name
        #2 rst = 1'b1;             // q falls at once, the rising edge's delay having run out

        #20 rb = 1'b1;             // b's path would end 20 on
        #2 ra = 1'b1;              // a's, the more recent, decides: 10 after a

        #20 wd = 4'ha;
        #5 wclk = 1'b1;            // the vector 4 after the edge, and the wrapped one with it,
                                   // its own path's 2 having run out by then
        #20 wclk = 1'b0;
        #20 $display("PASS module_paths");
        $finish;
    end
endmodule
