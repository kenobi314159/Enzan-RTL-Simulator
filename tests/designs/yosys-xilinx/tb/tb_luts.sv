// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's LUT1 to LUT6, LUT6_2, MUXF7 and MUXF8: every input of each against its INIT, settled,
// then the delays of their paths, one for each input: LUT6's from I0 and from I5, two inputs
// changing together taking the shorter, and MUXF7's from its select.

`timescale 1ns / 1ps

module tb_luts;
    localparam logic [63:0] INIT6 = 64'h9a3c_5f04_e7b2_46da;
    localparam logic [31:0] INIT5 = 32'h1f2e_3d4c;
    localparam logic [15:0] INIT4 = 16'hb4e1;
    localparam logic [7:0] INIT3 = 8'h96;
    localparam logic [3:0] INIT2 = 4'h6;
    localparam logic [1:0] INIT1 = 2'h1;

    logic [5:0] i = 6'd0;
    wire o1, o2, o3, o4, o5, o6, o6_2, o5_2, f7, f8;
    LUT1 #(.INIT(INIT1)) u1 (.O(o1), .I0(i[0]));
    LUT2 #(.INIT(INIT2)) u2 (.O(o2), .I0(i[0]), .I1(i[1]));
    LUT3 #(.INIT(INIT3)) u3 (.O(o3), .I0(i[0]), .I1(i[1]), .I2(i[2]));
    LUT4 #(.INIT(INIT4)) u4 (.O(o4), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]));
    LUT5 #(.INIT(INIT5)) u5 (.O(o5), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]), .I4(i[4]));
    LUT6 #(.INIT(INIT6)) u6 (
        .O(o6), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]), .I4(i[4]), .I5(i[5]));
    LUT6_2 #(.INIT(INIT6)) u6_2 (
        .O6(o6_2), .O5(o5_2), .I0(i[0]), .I1(i[1]), .I2(i[2]), .I3(i[3]), .I4(i[4]), .I5(i[5]));
    MUXF7 u_f7 (.O(f7), .I0(o5), .I1(o4), .S(i[5]));
    MUXF8 u_f8 (.O(f8), .I0(o3), .I1(f7), .S(i[4]));

    int failures = 0;
    task automatic expect1(int which, logic got, logic want);
        $display("%.3f: check %0d o=%b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    initial begin
        // Every input, settled.
        for (int n = 0; n < 64; n = n + 1) begin
            logic [9:0] want, got;
            logic f7_want;
            i = n[5:0];
            #2;
            f7_want = i[5] ? INIT4[i[3:0]] : INIT5[i[4:0]];
            want = {INIT1[i[0]], INIT2[i[1:0]], INIT3[i[2:0]], INIT4[i[3:0]], INIT5[i[4:0]],
                    INIT6[i], INIT6[i], INIT6[i[4:0]], f7_want,
                    i[4] ? f7_want : INIT3[i[2:0]]};
            got = {o1, o2, o3, o4, o5, o6, o6_2, o5_2, f7, f8};
            if (got !== want) begin
                $display("FAIL: inputs %b gave %b, should be %b", i, got, want);
                failures = failures + 1;
            end
        end

        // LUT6's delays: from I0 642 ps, from I5 127 ps, and from I0 and I1 at once the
        // shorter, I1's 631 ps. Each step changes its output.
        i = 6'd0;
        #2 expect1(1, o6, INIT6[0]);
        i = 6'd1;
        #0.640 expect1(2, o6, INIT6[0]);
        #0.004 expect1(3, o6, INIT6[1]);
        #2 i = 6'd33;
        #0.125 expect1(4, o6, INIT6[1]);
        #0.004 expect1(5, o6, INIT6[33]);
        #2 i = 6'd34;
        #0.629 expect1(6, o6, INIT6[33]);
        #0.004 expect1(7, o6, INIT6[34]);

        // MUXF7's select, 296 ps after the LUTs before it have settled.
        #2 i = 6'd2;
        #2 i = 6'd34;
        #0.294 expect1(8, f7, INIT5[2]);
        #0.004 expect1(9, f7, INIT4[2]);

        if (failures == 0) $display("PASS tb_luts");
        $finish;
    end
endmodule
