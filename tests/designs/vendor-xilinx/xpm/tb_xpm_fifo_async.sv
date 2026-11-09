// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's XPM asynchronous FIFO, 16 words of 32 bits, written at 100 MHz and read at about 71 MHz:
// out of reset, ten words written and read back in order on the other clock, each read's data and
// flags shown once settled, and the FIFO empty at the end.

`timescale 1ns / 1ps

module tb_xpm_fifo_async;
    logic wr_clk = 1'b0, rd_clk = 1'b0;
    always #5 wr_clk = ~wr_clk;
    always #7 rd_clk = ~rd_clk;

    logic rst = 1'b1, wr_en = 1'b0, rd_en = 1'b0;
    logic [31:0] din = '0;
    wire [31:0] dout;
    wire full, empty, wr_rst_busy, rd_rst_busy, data_valid;

    xpm_fifo_async #(.FIFO_WRITE_DEPTH(16), .WRITE_DATA_WIDTH(32), .READ_DATA_WIDTH(32),
                     .READ_MODE("std"), .FIFO_READ_LATENCY(1), .USE_ADV_FEATURES("1000"),
                     .CDC_SYNC_STAGES(2))
        u_fifo (.sleep(1'b0), .rst(rst), .wr_clk(wr_clk), .wr_en(wr_en), .din(din), .full(full),
                .prog_full(), .wr_data_count(), .overflow(), .wr_rst_busy(wr_rst_busy),
                .almost_full(), .wr_ack(), .rd_clk(rd_clk), .rd_en(rd_en), .dout(dout),
                .empty(empty), .prog_empty(), .rd_data_count(), .underflow(),
                .rd_rst_busy(rd_rst_busy), .almost_empty(), .data_valid(data_valid),
                .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr());

    int failures = 0;
    initial begin
        #200;
        @(negedge wr_clk) rst = 1'b0;
        wait (wr_rst_busy == 1'b0 && rd_rst_busy == 1'b0);
        @(negedge wr_clk);
        for (int word = 0; word < 10; word++) begin
            din = 32'hbeef_0000 + word;
            wr_en = 1'b1;
            @(negedge wr_clk);
        end
        wr_en = 1'b0;

        // Read once the reading side has seen the words, a few of its cycles later.
        repeat (6) @(negedge rd_clk);
        for (int word = 0; word < 10; word++) begin
            wait (empty == 1'b0);
            rd_en = 1'b1;
            @(negedge rd_clk);
            rd_en = 1'b0;
            $display("read %0d: %h valid=%b", word, dout, data_valid);
            if (dout != 32'hbeef_0000 + word || !data_valid) failures = failures + 1;
        end
        repeat (4) @(negedge rd_clk);
        $display("empty at the end: %b", empty);
        if (!empty) failures = failures + 1;

        if (failures == 0) $display("PASS tb_xpm_fifo_async");
        $finish;
    end
endmodule
