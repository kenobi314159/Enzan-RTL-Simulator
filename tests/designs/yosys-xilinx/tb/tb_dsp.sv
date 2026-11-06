// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's model of the DSP slice DSP48E1, fully pipelined, in the modes designs use most: a signed
// 25 by 18 multiply, a multiply added to C, C less the product, and accumulation of the product
// into P after a reset of it. Each operation's inputs are held while the pipeline settles, and
// accumulation is checked by what each clock adds.

`timescale 1ns / 1ps

module tb_dsp;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic signed [24:0] a = '0;
    logic signed [17:0] b = '0;
    logic signed [47:0] c = '0;
    logic [6:0] opmode = 7'b0000101;
    logic [3:0] alumode = 4'b0000;
    logic rstp = 1'b0;
    wire signed [47:0] p;
    DSP48E1 #(.AREG(1), .BREG(1), .CREG(1), .MREG(1), .PREG(1), .OPMODEREG(1), .ALUMODEREG(1))
        u_dsp (
            .CLK(clk), .A({{5{a[24]}}, a}), .B(b), .C(c), .D(25'd0), .OPMODE(opmode),
            .ALUMODE(alumode), .INMODE(5'b00000), .CARRYIN(1'b0), .CARRYINSEL(3'b000),
            .ACIN(30'd0), .BCIN(18'd0), .PCIN(48'd0), .CARRYCASCIN(1'b0), .MULTSIGNIN(1'b0),
            .CEA1(1'b1), .CEA2(1'b1), .CEAD(1'b1), .CEALUMODE(1'b1), .CEB1(1'b1), .CEB2(1'b1),
            .CEC(1'b1), .CECARRYIN(1'b1), .CECTRL(1'b1), .CED(1'b1), .CEINMODE(1'b1), .CEM(1'b1),
            .CEP(1'b1), .RSTA(1'b0), .RSTALLCARRYIN(1'b0), .RSTALUMODE(1'b0), .RSTB(1'b0),
            .RSTC(1'b0), .RSTCTRL(1'b0), .RSTD(1'b0), .RSTINMODE(1'b0), .RSTM(1'b0), .RSTP(rstp),
            .P(p), .ACOUT(), .BCOUT(), .CARRYCASCOUT(), .CARRYOUT(), .MULTSIGNOUT(), .OVERFLOW(),
            .PATTERNBDETECT(), .PATTERNDETECT(), .PCOUT(), .UNDERFLOW());

    int failures = 0;
    task automatic expect48(int which, logic signed [47:0] want);
        $display("%0d: check %0d p=%h", $time, which, p);
        if (p !== want) begin
            $display("FAIL: check %0d gave %h, should be %h", which, p, want);
            failures = failures + 1;
        end
    endtask

    // The inputs change 2 ns after a falling edge, and P is checked once the pipeline, three
    // registers deep, has settled.
    task automatic operate(int which, logic signed [24:0] a_in, logic signed [17:0] b_in,
                           logic signed [47:0] c_in, logic [6:0] mode, logic [3:0] alu,
                           logic signed [47:0] want);
        @(negedge clk);
        #2;
        a = a_in;
        b = b_in;
        c = c_in;
        opmode = mode;
        alumode = alu;
        repeat (5) @(posedge clk);
        #2 expect48(which, want);
    endtask

    localparam logic [6:0] MULTIPLY = 7'b0000101, PLUS_C = 7'b0110101, ACCUMULATE = 7'b0100101;
    localparam logic [3:0] ADD = 4'b0000, Z_MINUS = 4'b0011;

    initial begin
        operate(1, 25'sd1234, 18'sd567, 48'sd0, MULTIPLY, ADD, 48'sd699678);
        operate(2, -25'sd4096, 18'sd3, 48'sd0, MULTIPLY, ADD, -48'sd12288);
        operate(3, 25'sd1000, -18'sd1000, 48'sd5000000, PLUS_C, ADD, 48'sd4000000);
        operate(4, 25'sd77, 18'sd11, 48'sd1000, PLUS_C, Z_MINUS, 48'sd153);
        operate(5, 25'sh0ff_ffff, 18'sh1_ffff, 48'sd0, MULTIPLY, ADD, 48'sd2199006347265);

        // Accumulation: P is reset, then gains the product at every clock.
        @(negedge clk);
        #2;
        a = 25'sd21;
        b = 18'sd2;
        opmode = MULTIPLY;
        rstp = 1'b1;
        repeat (4) @(posedge clk);
        #2;
        rstp = 1'b0;
        opmode = ACCUMULATE;
        repeat (2) @(posedge clk);
        #2 begin
            logic signed [47:0] held;
            held = p;
            @(posedge clk);
            #2 expect48(6, held + 48'sd42);
            @(posedge clk);
            #2 expect48(7, held + 48'sd84);
        end

        if (failures == 0) $display("PASS tb_dsp");
        $finish;
    end
endmodule
