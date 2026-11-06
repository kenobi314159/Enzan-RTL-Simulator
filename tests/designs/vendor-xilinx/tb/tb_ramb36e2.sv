// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's UltraScale block RAM RAMB36E2 in true dual-port mode, 36 bits wide on both ports: its
// contents from INIT_FILE read through port A, a word written through port B and read back through
// A, and the 100 ps delay from the clock to the output.

`timescale 1ns / 1ps

module tb_ramb36e2;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic [9:0] addrA = 10'd0, addrB = 10'd0;
    logic [35:0] dinB = 36'd0;
    logic weB = 1'b0;
    wire [31:0] doutA;
    wire [3:0] doutPA;

    RAMB36E2 #(.READ_WIDTH_A(36), .WRITE_WIDTH_A(36), .READ_WIDTH_B(36), .WRITE_WIDTH_B(36),
               .DOA_REG(0), .DOB_REG(0), .INIT_FILE("block_ram.mem")) u_ram (
        .CASDOUTA(), .CASDOUTB(), .CASDOUTPA(), .CASDOUTPB(), .CASOUTDBITERR(),
        .CASOUTSBITERR(), .DBITERR(), .ECCPARITY(), .RDADDRECC(), .SBITERR(),
        .DOUTADOUT(doutA), .DOUTPADOUTP(doutPA), .DOUTBDOUT(), .DOUTPBDOUTP(),
        .ADDRARDADDR({addrA, 5'b00000}), .ADDRBWRADDR({addrB, 5'b00000}), .ADDRENA(1'b1),
        .ADDRENB(1'b1), .CASDIMUXA(1'b0), .CASDIMUXB(1'b0), .CASDINA(32'd0), .CASDINB(32'd0),
        .CASDINPA(4'd0), .CASDINPB(4'd0), .CASDOMUXA(1'b0), .CASDOMUXB(1'b0),
        .CASDOMUXEN_A(1'b1), .CASDOMUXEN_B(1'b1), .CASINDBITERR(1'b0), .CASINSBITERR(1'b0),
        .CASOREGIMUXA(1'b0), .CASOREGIMUXB(1'b0), .CASOREGIMUXEN_A(1'b1),
        .CASOREGIMUXEN_B(1'b1), .CLKARDCLK(clk), .CLKBWRCLK(clk), .DINADIN(32'd0),
        .DINBDIN(dinB[31:0]), .DINPADINP(4'd0), .DINPBDINP(dinB[35:32]), .ECCPIPECE(1'b1),
        .ENARDEN(1'b1), .ENBWREN(1'b1), .INJECTDBITERR(1'b0), .INJECTSBITERR(1'b0),
        .REGCEAREGCE(1'b1), .REGCEB(1'b1), .RSTRAMARSTRAM(1'b0), .RSTRAMB(1'b0),
        .RSTREGARSTREG(1'b0), .RSTREGB(1'b0), .SLEEP(1'b0), .WEA(4'b0000),
        .WEBWE({4'b0000, {4{weB}}}));

    logic [35:0] contents [0:7];
    initial begin
        contents[0] = 36'h123456789;
        contents[1] = 36'habcdef012;
        contents[2] = 36'h0fedcba98;
        contents[3] = 36'hf00000001;
        contents[4] = 36'h555555555;
        contents[5] = 36'haaaaaaaaa;
        contents[6] = 36'h000000000;
        contents[7] = 36'h876543210;
    end

    int failures = 0;
    task automatic readA(int address, logic [35:0] want);
        @(negedge clk) addrA = 10'(address);
        @(negedge clk);
        $display("read %0d: %h", address, {doutPA, doutA});
        if ({doutPA, doutA} !== want) failures = failures + 1;
    endtask

    initial begin
        #200;
        for (int address = 0; address < 8; address++) readA(address, contents[address]);
        readA(100, 36'bx);    // past the file, which this model leaves unknown, unlike RAMB36E1

        // A write through B, read back through A.
        @(negedge clk);
        addrB = 10'd3;
        dinB = 36'h9_8765_4321;
        weB = 1'b1;
        @(negedge clk) weB = 1'b0;
        readA(3, 36'h9_8765_4321);

        // The output is the old word until 100 ps after the edge.
        @(negedge clk) addrA = 10'd1;
        @(posedge clk);
        #0.09 $display("before: %h", {doutPA, doutA});
        if ({doutPA, doutA} !== 36'h9_8765_4321) failures = failures + 1;
        #0.02 $display("after: %h", {doutPA, doutA});
        if ({doutPA, doutA} !== contents[1]) failures = failures + 1;

        if (failures == 0) $display("PASS tb_ramb36e2");
        $finish;
    end
endmodule
