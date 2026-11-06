// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's bidirectional buffer IOBUF on a pad with a PULLUP: driving the pad while its tristate
// control is low, released to the pull-up while it is high, and reading back whatever the pad
// carries, the testbench driving it from outside too.

`timescale 1ns / 1ps

module tb_iobuf;
    logic data = 1'b0, tristate = 1'b1;
    logic outside = 1'bz;
    wire pad, read;
    IOBUF u_iobuf (.O(read), .IO(pad), .I(data), .T(tristate));
    PULLUP u_pullup (.O(pad));
    assign pad = outside;

    int failures = 0;
    task automatic expect2(int which, logic wantPad, logic wantRead);
        $display("check %0d pad=%b read=%b", which, pad, read);
        if (pad !== wantPad || read !== wantRead) failures = failures + 1;
    endtask

    initial begin
        #200 expect2(1, 1'b1, 1'b1);      // released, pulled up
        tristate = 1'b0;
        #10 expect2(2, 1'b0, 1'b0);       // driven low
        data = 1'b1;
        #10 expect2(3, 1'b1, 1'b1);       // driven high
        tristate = 1'b1;
        outside = 1'b0;
        #10 expect2(4, 1'b0, 1'b0);       // driven from outside, read back
        outside = 1'bz;
        #10 expect2(5, 1'b1, 1'b1);       // the pull-up again
        tristate = 1'b0;
        data = 1'b0;
        outside = 1'b1;
        #10 expect2(6, 1'bx, 1'bx);       // both driving, disagreeing

        if (failures == 0) $display("PASS tb_iobuf");
        $finish;
    end
endmodule
