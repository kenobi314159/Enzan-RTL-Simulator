// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FIFO of tests/golden/vhdl/fifo.vhd driven from SystemVerilog by the stimulus of
// tops/fifo.vhd, so that every port of it crosses the language boundary on every cycle and the
// two rates differ by what the crossing costs. Its checksum is tops/fifo.vhd's. The cycle count
// is fixed in the source for the reason pipeline.sv gives.

module bench #(
    parameter int CYCLES = 200000
);
    logic clk = 1'b0;
    logic rst = 1'b1;
    int   cycle_count = 0;

    // A Galois LFSR, whose low two bits decide whether to push and whether to pop.
    logic [31:0] lfsr = 32'h1234_5678;
    logic [31:0] checksum = '0;

    logic        push;
    logic        pop;
    logic [31:0] data_out;
    logic        full;
    logic        empty;

    fifo #(.width(32), .depth_log2(4)) uut (
        .clk(clk),
        .rst(rst),
        .push(push),
        .data_in(lfsr),
        .pop(pop),
        .data_out(data_out),
        .full(full),
        .empty(empty)
    );

    always #5 clk = ~clk;

    assign push = lfsr[0];
    assign pop  = lfsr[1];

    always_ff @(posedge clk) begin
        cycle_count <= cycle_count + 1;
        if (cycle_count == 2) rst <= 1'b0;
        lfsr <= (lfsr >> 1) ^ (lfsr[0] ? 32'h8020_0003 : 32'h0);
        if (!rst && pop && !empty) checksum <= {checksum[30:0], checksum[31]} ^ data_out;
    end

    initial begin
        while (cycle_count < CYCLES)
            @(posedge clk);

        // Printed so that the harness can confirm the run did the work it was asked for, and
        // so that a change which accidentally optimises the design away is visible.
        $display("cycles=%0d checksum=%h", cycle_count, checksum);
        $finish;
    end
endmodule
