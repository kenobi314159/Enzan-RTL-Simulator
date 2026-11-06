// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's XPM synchronous FIFO, 16 words of 32 bits in standard read mode: out of reset, ten words
// written and then read back in order, its empty, full and count flags shown at each step, and an
// underflow on a read of the empty FIFO.

`timescale 1ns / 1ps

module tb_xpm_fifo_sync;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic rst = 1'b1, wr_en = 1'b0, rd_en = 1'b0;
    logic [31:0] din = '0;
    wire [31:0] dout;
    wire full, empty, wr_rst_busy, rd_rst_busy, underflow, data_valid;
    wire [4:0] wr_data_count, rd_data_count;

    xpm_fifo_sync #(.FIFO_WRITE_DEPTH(16), .WRITE_DATA_WIDTH(32), .READ_DATA_WIDTH(32),
                    .READ_MODE("std"), .FIFO_READ_LATENCY(1), .USE_ADV_FEATURES("1707"),
                    .WR_DATA_COUNT_WIDTH(5), .RD_DATA_COUNT_WIDTH(5))
        u_fifo (.sleep(1'b0), .rst(rst), .wr_clk(clk), .wr_en(wr_en), .din(din), .full(full),
                .prog_full(), .wr_data_count(wr_data_count), .overflow(), .wr_rst_busy(wr_rst_busy),
                .almost_full(), .wr_ack(), .rd_en(rd_en), .dout(dout), .empty(empty),
                .prog_empty(), .rd_data_count(rd_data_count), .underflow(underflow),
                .rd_rst_busy(rd_rst_busy), .almost_empty(), .data_valid(data_valid),
                .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr());

    int failures = 0;
    initial begin
        #200;
        @(negedge clk) rst = 1'b0;
        wait (wr_rst_busy == 1'b0 && rd_rst_busy == 1'b0);
        @(negedge clk);
        $display("out of reset: empty=%b full=%b", empty, full);

        for (int word = 0; word < 10; word++) begin
            din = 32'hface_0000 + word;
            wr_en = 1'b1;
            @(negedge clk);
            $display("wrote %0d: empty=%b full=%b count=%0d", word, empty, full, wr_data_count);
        end
        wr_en = 1'b0;
        repeat (3) @(negedge clk);

        for (int word = 0; word < 10; word++) begin
            rd_en = 1'b1;
            @(negedge clk);
            rd_en = 1'b0;
            $display("read %0d: %h valid=%b empty=%b count=%0d", word, dout, data_valid, empty,
                     rd_data_count);
            if (dout != 32'hface_0000 + word || !data_valid) failures = failures + 1;
        end
        rd_en = 1'b1;
        @(negedge clk) rd_en = 1'b0;
        $display("read empty: underflow=%b empty=%b", underflow, empty);
        if (!underflow || !empty) failures = failures + 1;

        if (failures == 0) $display("PASS tb_xpm_fifo_sync");
        $finish;
    end
endmodule
