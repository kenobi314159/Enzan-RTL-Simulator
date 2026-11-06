// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's UltraScale DSP slice DSP48E2, fully pipelined, in the modes designs use most: a signed 27
// by 18 multiply, a multiply added to C, C less the product, and accumulation of the product into P.
// Each operation's inputs are held while the pipeline settles, and accumulation is checked by what
// each clock adds.

`timescale 1ns / 1ps

module tb_dsp48e2;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic signed [26:0] a = '0;
    logic signed [17:0] b = '0;
    logic signed [47:0] c = '0;
    logic [8:0] opmode = 9'b000000101;
    logic [3:0] alumode = 4'b0000;
    logic rstp = 1'b0;
    wire signed [47:0] p;

    DSP48E2 #(.AREG(1), .BREG(1), .CREG(1), .MREG(1), .PREG(1), .USE_MULT("MULTIPLY"))
        u_dsp (.ACOUT(), .BCOUT(), .CARRYCASCOUT(), .CARRYOUT(), .MULTSIGNOUT(), .OVERFLOW(),
               .P(p), .PATTERNBDETECT(), .PATTERNDETECT(), .PCOUT(), .UNDERFLOW(), .XOROUT(),
               .A({{3{a[26]}}, a}), .ACIN(30'd0), .ALUMODE(alumode), .B(b), .BCIN(18'd0), .C(c),
               .CARRYCASCIN(1'b0), .CARRYIN(1'b0), .CARRYINSEL(3'b000), .CEA1(1'b1),
               .CEA2(1'b1), .CEAD(1'b1), .CEALUMODE(1'b1), .CEB1(1'b1), .CEB2(1'b1),
               .CEC(1'b1), .CECARRYIN(1'b1), .CECTRL(1'b1), .CED(1'b1), .CEINMODE(1'b1),
               .CEM(1'b1), .CEP(1'b1), .CLK(clk), .D(27'd0), .INMODE(5'b00000),
               .MULTSIGNIN(1'b0), .OPMODE(opmode), .PCIN(48'd0), .RSTA(1'b0),
               .RSTALLCARRYIN(1'b0), .RSTALUMODE(1'b0), .RSTB(1'b0), .RSTC(1'b0),
               .RSTCTRL(1'b0), .RSTD(1'b0), .RSTINMODE(1'b0), .RSTM(1'b0), .RSTP(rstp));

    logic signed [47:0] previous;
    int failures = 0;
    task automatic settle(int which, logic signed [47:0] want);
        repeat (5) @(negedge clk);
        $display("check %0d: p=%0d", which, p);
        if (p !== want) begin
            $display("FAIL: check %0d gave %0d, should be %0d", which, p, want);
            failures = failures + 1;
        end
    endtask

    initial begin
        #200;
        @(negedge clk);
        a = 27'sd12345;
        b = -18'sd678;
        settle(1, 48'(a * b));                              // multiply
        a = -27'sd40000000;
        b = 18'sd131071;
        settle(2, 48'(a * b));

        c = 48'sd987654321;
        opmode = 9'b000110101;                              // C + A*B
        settle(3, c + 48'(a * b));
        alumode = 4'b0011;                                  // C - A*B
        settle(4, c - 48'(a * b));

        // Accumulate: P + A*B on every clock.
        alumode = 4'b0000;
        opmode = 9'b000100101;
        a = 27'sd3;
        b = 18'sd7;
        repeat (5) @(negedge clk);
        for (int step = 0; step < 4; step++) begin
            previous = p;
            @(negedge clk);
            $display("accumulated: p=%0d", p);
            if (p - previous !== 48'sd21) failures = failures + 1;
        end

        if (failures == 0) $display("PASS tb_dsp48e2");
        $finish;
    end
endmodule
