// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's XPM clock-domain crossings from a 100 MHz domain into one of about 71 MHz: a single bit, a
// Gray-coded count, a word sent by handshake, and a pulse. Each value is shown once the destination
// has had time to take it, and must then be what was sent.

`timescale 1ns / 1ps

module tb_xpm_cdc;
    logic src_clk = 1'b0, dest_clk = 1'b0;
    always #5 src_clk = ~src_clk;
    always #7 dest_clk = ~dest_clk;

    logic bit_in = 1'b0;
    wire bit_out;
    xpm_cdc_single #(.DEST_SYNC_FF(4), .SRC_INPUT_REG(1)) u_single (
        .src_clk(src_clk), .src_in(bit_in), .dest_clk(dest_clk), .dest_out(bit_out));

    logic [3:0] count = 4'd0;
    wire [3:0] count_out;
    xpm_cdc_gray #(.DEST_SYNC_FF(4), .WIDTH(4)) u_gray (
        .src_clk(src_clk), .src_in_bin(count), .dest_clk(dest_clk), .dest_out_bin(count_out));

    logic [15:0] word = 16'd0;
    logic send = 1'b0;
    wire received, request;
    wire [15:0] word_out;
    logic ack = 1'b0;
    xpm_cdc_handshake #(.DEST_EXT_HSK(1), .WIDTH(16)) u_handshake (
        .src_clk(src_clk), .src_in(word), .src_send(send), .src_rcv(received),
        .dest_clk(dest_clk), .dest_out(word_out), .dest_req(request), .dest_ack(ack));
    always @(posedge dest_clk) ack <= request;

    logic pulse = 1'b0, rst = 1'b1;
    wire pulse_out;
    xpm_cdc_pulse #(.DEST_SYNC_FF(4), .RST_USED(1)) u_pulse (
        .src_clk(src_clk), .src_pulse(pulse), .dest_clk(dest_clk), .src_rst(rst), .dest_rst(rst),
        .dest_pulse(pulse_out));
    int pulses = 0;
    always @(posedge dest_clk) if (pulse_out) pulses = pulses + 1;

    int failures = 0;
    initial begin
        #200;
        @(negedge src_clk) rst = 1'b0;
        repeat (4) @(negedge src_clk);

        bit_in = 1'b1;
        repeat (10) @(negedge dest_clk);
        $display("single: %b", bit_out);
        if (bit_out != 1'b1) failures = failures + 1;

        for (int step = 0; step < 6; step++) begin
            @(negedge src_clk) count = count + 4'd1;
            repeat (8) @(negedge dest_clk);
            $display("gray: %0d", count_out);
            if (count_out != count) failures = failures + 1;
        end

        @(negedge src_clk);
        word = 16'h5a3c;
        send = 1'b1;
        wait (received == 1'b1);
        @(negedge src_clk) send = 1'b0;
        $display("handshake: %h", word_out);
        if (word_out != 16'h5a3c) failures = failures + 1;

        repeat (3) begin
            @(negedge src_clk) pulse = 1'b1;
            @(negedge src_clk) pulse = 1'b0;
            repeat (12) @(negedge src_clk);
        end
        repeat (10) @(negedge dest_clk);
        $display("pulses: %0d", pulses);
        if (pulses != 3) failures = failures + 1;

        if (failures == 0) $display("PASS tb_xpm_cdc");
        $finish;
    end
endmodule
