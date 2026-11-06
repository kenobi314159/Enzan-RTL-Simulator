// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's buffers and inverter: IBUF, OBUF and BUFG passing their input, BUFG 96 ps later and INV
// 127 ps later; OBUFT and IOBUF letting a pad go to z, the pad of IOBUF driven from outside and
// read back; BUFGCTRL choosing between two clocks by its selects and enables, held at its
// INIT_OUT when neither is chosen; and BUFHCE gating its input by its clock enable.

`timescale 1ns / 1ps

module tb_buffers;
    logic i = 1'b0, t = 1'b1;
    wire o_ibuf, o_obuf, o_bufg, o_inv, o_obuft, o_iobuf;
    IBUF u_ibuf (.O(o_ibuf), .I(i));
    OBUF u_obuf (.O(o_obuf), .I(i));
    BUFG u_bufg (.O(o_bufg), .I(i));
    INV u_inv (.O(o_inv), .I(i));
    OBUFT u_obuft (.O(o_obuft), .I(i), .T(t));

    // The pad of IOBUF, which the testbench drives while the buffer's output is off.
    logic pad_drive = 1'b0, pad_driving = 1'b0;
    wire pad;
    assign pad = pad_driving ? pad_drive : 1'bz;
    IOBUF u_iobuf (.IO(pad), .O(o_iobuf), .I(i), .T(t));

    logic i0 = 1'b0, i1 = 1'b1, s0 = 1'b1, s1 = 1'b0, ce0 = 1'b1, ce1 = 1'b1;
    wire o_ctrl, o_hce;
    BUFGCTRL #(.INIT_OUT(1'b1)) u_ctrl (
        .O(o_ctrl), .I0(i0), .I1(i1), .S0(s0), .S1(s1), .CE0(ce0), .CE1(ce1), .IGNORE0(1'b0),
        .IGNORE1(1'b0));
    BUFHCE u_hce (.O(o_hce), .I(i0), .CE(ce0));

    int failures = 0;
    task automatic expect8(int which, logic [7:0] got, logic [7:0] want);
        $display("%.3f: check %0d %b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    // The buffers, in one vector: IBUF, OBUF, BUFG, INV, OBUFT, the pad, IOBUF's O, and 0.
    function automatic logic [7:0] buffers();
        return {o_ibuf, o_obuf, o_bufg, o_inv, o_obuft, pad, o_iobuf, 1'b0};
    endfunction

    initial begin
        #1 expect8(1, buffers(), 8'b0001zzz0);
        i = 1'b1;
        #0.090 expect8(2, buffers(), 8'b1101zzz0);    // BUFG and INV not yet
        #0.010 expect8(3, buffers(), 8'b1111zzz0);    // BUFG at 96 ps
        #0.030 expect8(4, buffers(), 8'b1110zzz0);    // INV at 127 ps
        t = 1'b0;
        #1 expect8(5, buffers(), 8'b11101110);        // driving the pad
        t = 1'b1;
        pad_driving = 1'b1;
        #1 expect8(6, buffers(), 8'b1110z000);        // the pad driven from outside
        pad_drive = 1'b1;
        #1 expect8(7, buffers(), 8'b1110z110);

        // BUFGCTRL: I0 by S0, then I1 by S1, then neither, then I0 disabled.
        expect8(8, {o_ctrl, o_hce, 6'b0}, 8'b00000000);
        i0 = 1'b1;
        #1 expect8(9, {o_ctrl, o_hce, 6'b0}, 8'b11000000);
        {s0, s1} = 2'b01;
        i1 = 1'b0;
        #1 expect8(10, {o_ctrl, o_hce, 6'b0}, 8'b01000000);
        s1 = 1'b0;
        #1 expect8(11, {o_ctrl, o_hce, 6'b0}, 8'b11000000);   // INIT_OUT
        s0 = 1'b1;
        ce0 = 1'b0;
        #1 expect8(12, {o_ctrl, o_hce, 6'b0}, 8'b10000000);   // INIT_OUT, and BUFHCE's 0

        if (failures == 0) $display("PASS tb_buffers");
        $finish;
    end
endmodule
