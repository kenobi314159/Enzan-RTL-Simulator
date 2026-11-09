// SPDX-License-Identifier: GPL-3.0-or-later
//
// Delayed writes to part of a signal, as AMD's distributed RAMs and block RAMs write a memory
// word and its clock managers a bit of a mux: nonblocking to an element at an index known only as
// the process runs, each arriving when its delay says; and a continuous assignment of one bit with
// a delay, inertially, so a pulse shorter than it never arrives.

module tb;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic [7:0] mem [0:3];
    logic [1:0] addr = 2'd0;
    logic [7:0] data = 8'h00;
    logic write = 1'b0;
    always @(posedge clk)
        if (write) mem[addr] <= #2 data;

    logic [3:0] mux = 4'b0000;
    logic source = 1'b0;
    always @(source) mux[1] <= #3 source;

    logic a = 1'b0;
    wire [1:0] lp;
    assign #4 lp[0] = a;
    assign lp[1] = 1'b1;

    initial begin
        for (int i = 0; i < 4; i++) mem[i] = 8'h00;
        #1 write = 1'b1;
        addr = 2'd1;
        data = 8'h11;
        #10 addr = 2'd2;
        data = 8'h22;
        #1 $display("t=%0d mem1=%h mem2=%h", $time, mem[1], mem[2]);
        #2 $display("t=%0d mem1=%h mem2=%h", $time, mem[1], mem[2]);
        #5 $display("t=%0d mem1=%h mem2=%h", $time, mem[1], mem[2]);
        write = 1'b0;

        source = 1'b1;
        #1 source = 1'b0;
        #1 $display("t=%0d mux=%b", $time, mux);
        #2 $display("t=%0d mux=%b", $time, mux);
        #1 $display("t=%0d mux=%b", $time, mux);

        a = 1'b1;
        #2 a = 1'b0;
        #5 $display("t=%0d lp=%b", $time, lp);
        a = 1'b1;
        #5 $display("t=%0d lp=%b", $time, lp);
        $display("PASS delayed_parts");
        $finish;
    end
endmodule
