// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's shift register SRL16E against a shift register of the testbench's own: its initial
// contents, a shift on each enabled clock, and a read at an address that changes between clocks.

`timescale 1ns / 1ps

module tb_srl16;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic d = 1'b0, ce = 1'b1;
    logic [3:0] a = 4'd0;
    wire q;
    SRL16E #(.INIT(16'ha5c3)) u16 (.Q(q), .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .CE(ce),
                                  .CLK(clk), .D(d));

    logic [15:0] shadow = 16'ha5c3;
    always @(posedge clk)
        if (ce) shadow <= {shadow[14:0], d};

    int failures = 0;
    task automatic compare(int which);
        if (q !== shadow[a]) begin
            $display("FAIL: check %0d at a=%0d gave %b", which, a, q);
            failures = failures + 1;
        end
    endtask

    initial begin
        // Initial contents, read at every address before anything shifts.
        for (int address = 0; address < 16; address++) begin
            a = address[3:0];
            #0.1 compare(address);
        end

        for (int step = 0; step < 80; step++) begin
            @(negedge clk);
            d = step[0] ^ step[2];
            ce = step % 7 != 3;
            a = 4'(step * 11);
            #1 compare(100 + step);
        end
        $display("q=%b", q);

        if (failures == 0) $display("PASS tb_srl16");
        $finish;
    end
endmodule
