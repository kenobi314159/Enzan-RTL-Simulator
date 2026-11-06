// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's XPM AXI-Stream FIFO, 16 beats of 32 bits on one clock: a packet of eight beats sent while
// the receiver holds off, then taken in order with its last beat marked, the handshake shown as it
// goes.

`timescale 1ns / 1ps

module tb_xpm_fifo_axis;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic aresetn = 1'b0, tvalid = 1'b0, tlast = 1'b0, tready_out = 1'b0;
    logic [31:0] tdata = '0;
    wire tready, tvalid_out, tlast_out;
    wire [31:0] tdata_out;

    xpm_fifo_axis #(.CLOCKING_MODE("common_clock"), .FIFO_DEPTH(16), .TDATA_WIDTH(32))
        u_fifo (.s_aresetn(aresetn), .s_aclk(clk), .m_aclk(clk), .s_axis_tvalid(tvalid),
                .s_axis_tready(tready), .s_axis_tdata(tdata), .s_axis_tstrb(4'hf),
                .s_axis_tkeep(4'hf), .s_axis_tlast(tlast), .s_axis_tid(1'b0),
                .s_axis_tdest(1'b0), .s_axis_tuser(1'b0), .m_axis_tvalid(tvalid_out),
                .m_axis_tready(tready_out), .m_axis_tdata(tdata_out), .m_axis_tstrb(),
                .m_axis_tkeep(), .m_axis_tlast(tlast_out), .m_axis_tid(), .m_axis_tdest(),
                .m_axis_tuser(), .prog_full_axis(), .wr_data_count_axis(), .almost_full_axis(),
                .prog_empty_axis(), .rd_data_count_axis(), .almost_empty_axis(),
                .injectsbiterr_axis(1'b0), .injectdbiterr_axis(1'b0), .sbiterr_axis(),
                .dbiterr_axis());

    int failures = 0;
    initial begin
        #200;
        @(negedge clk) aresetn = 1'b1;
        wait (tready == 1'b1);
        @(negedge clk);

        for (int beat = 0; beat < 8; beat++) begin
            tdata = 32'h0dd0_0000 + beat;
            tlast = beat == 7;
            tvalid = 1'b1;
            @(negedge clk);
            $display("sent %0d: tready=%b", beat, tready);
        end
        tvalid = 1'b0;
        tlast = 1'b0;
        repeat (4) @(negedge clk);

        for (int beat = 0; beat < 8; beat++) begin
            wait (tvalid_out == 1'b1);
            $display("received %0d: %h last=%b", beat, tdata_out, tlast_out);
            if (tdata_out != 32'h0dd0_0000 + beat || tlast_out != (beat == 7))
                failures = failures + 1;
            tready_out = 1'b1;
            @(negedge clk) tready_out = 1'b0;
        end
        @(negedge clk);
        $display("valid at the end: %b", tvalid_out);
        if (tvalid_out) failures = failures + 1;

        if (failures == 0) $display("PASS tb_xpm_fifo_axis");
        $finish;
    end
endmodule
