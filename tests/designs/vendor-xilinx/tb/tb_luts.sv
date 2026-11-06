// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's LUT1 to LUT6 over every input combination, each with an INIT that is no simple function,
// and LUT2 and LUT6 with an unknown input, whose output is known only where the INIT does not
// depend on it.

`timescale 1ns / 1ps

module tb_luts;
    // Not initialised: an initial value makes no event (IEEE 1800 section 6.8), so the models,
    // which wait for their inputs to change, would not see the first combination.
    logic [5:0] i;
    wire [6:1] o;
    LUT1 #(.INIT(2'b10)) u1 (.O(o[1]), .I0(i[0]));
    LUT2 #(.INIT(4'b0110)) u2 (.O(o[2]), .I0(i[0]), .I1(i[1]));
    LUT3 #(.INIT(8'hd8)) u3 (.O(o[3]), .I0(i[0]), .I1(i[1]), .I2(i[2]));
    LUT4 #(.INIT(16'h6996)) u4 (.O(o[4]), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]));
    LUT5 #(.INIT(32'hdeadbeef)) u5 (.O(o[5]), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]),
                                   .I4(i[4]));
    LUT6 #(.INIT(64'h0123456789abcdef)) u6 (.O(o[6]), .I0(i[0]), .I1(i[1]), .I2(i[2]),
                                            .I3(i[3]), .I4(i[4]), .I5(i[5]));

    localparam logic [1:0] Init1 = 2'b10;
    localparam logic [3:0] Init2 = 4'b0110;
    localparam logic [7:0] Init3 = 8'hd8;
    localparam logic [15:0] Init4 = 16'h6996;
    localparam logic [31:0] Init5 = 32'hdeadbeef;
    localparam logic [63:0] Init6 = 64'h0123456789abcdef;

    int failures = 0;
    initial begin
        for (int value = 0; value < 64; value++) begin
            i = value[5:0];
            #1;
            if (o !== {Init6[i], Init5[i[4:0]], Init4[i[3:0]], Init3[i[2:0]], Init2[i[1:0]],
                       Init1[i[0]]}) begin
                $display("FAIL: i=%b gave %b", i, o);
                failures = failures + 1;
            end
        end
        $display("every input checked");

        // INIT 0110 is i0 ^ i1, which an unknown i1 leaves unknown; LUT6 with only I5 unknown
        // is known where the halves of its INIT agree.
        i = 6'b0000x0;
        #1 $display("lut2 with x: %b", o[2]);
        if (o[2] !== 1'bx) failures = failures + 1;
        i = 6'bx00000;
        #1 $display("lut6 with x: %b", o[6]);
        if (o[6] !== (Init6[0] == Init6[32] ? Init6[0] : 1'bx)) failures = failures + 1;

        if (failures == 0) $display("PASS tb_luts");
        $finish;
    end
endmodule
