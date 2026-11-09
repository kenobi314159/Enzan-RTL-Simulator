// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's unimacro BRAM_SDP_MACRO, a simple dual-port block RAM of 36 Kb with 32-bit words, over
// RAMB36E1: words written through the write port and read back through the read port, a write of
// only some bytes, and the 100 ps delay from the read clock to the output.

`timescale 1ns / 1ps

module tb_bram_sdp_macro;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic [9:0] wraddr = 10'd0, rdaddr = 10'd0;
    logic [31:0] di = 32'd0;
    logic [3:0] we = 4'b0000;
    logic wren = 1'b0;
    wire [31:0] dout;
    BRAM_SDP_MACRO #(.BRAM_SIZE("36Kb"), .DEVICE("7SERIES"), .WRITE_WIDTH(32), .READ_WIDTH(32),
                     .DO_REG(0), .WRITE_MODE("READ_FIRST")) u_ram (
        .DO(dout), .DI(di), .RDADDR(rdaddr), .RDCLK(clk), .RDEN(1'b1), .REGCE(1'b1),
        .RST(1'b0), .WE(we), .WRADDR(wraddr), .WRCLK(clk), .WREN(wren));

    int failures = 0;
    task automatic write(int address, logic [31:0] data, logic [3:0] bytes);
        @(negedge clk);
        wraddr = 10'(address);
        di = data;
        we = bytes;
        wren = 1'b1;
        @(negedge clk) wren = 1'b0;
    endtask
    task automatic read(int address, logic [31:0] want);
        @(negedge clk) rdaddr = 10'(address);
        @(negedge clk);
        $display("read %0d: %h", address, dout);
        if (dout !== want) failures = failures + 1;
    endtask

    initial begin
        #200;
        for (int address = 0; address < 8; address++)
            write(address, 32'h1111_1111 * (address + 1), 4'b1111);
        for (int address = 0; address < 8; address++) read(address, 32'h1111_1111 * (address + 1));

        // Bytes 0 and 2 only.
        write(2, 32'hdead_beef, 4'b0101);
        read(2, 32'h33ad_33ef);

        // The output is the old word until 100 ps after the edge.
        @(negedge clk) rdaddr = 10'd5;
        @(posedge clk);
        #0.09 $display("before: %h", dout);
        if (dout !== 32'h33ad_33ef) failures = failures + 1;
        #0.02 $display("after: %h", dout);
        if (dout !== 32'h6666_6666) failures = failures + 1;

        if (failures == 0) $display("PASS tb_bram_sdp_macro");
        $finish;
    end
endmodule
