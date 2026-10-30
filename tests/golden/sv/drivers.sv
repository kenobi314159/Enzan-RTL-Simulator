// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drivers of disjoint bits of one signal, which are accepted rather than reported as multiple
// driving: overlap, not sharing a signal, is what makes a signal multiply driven.
//
// The case that forces it is ordinary RTL: a parameterised number of instances whose outputs
// are bits, and slices, of one array. Every instance drives its own bits and no other, so none
// of them is in conflict with another. The two `assign`s to halves of `split` are the same
// thing written by hand.

module channel #(
    parameter logic [3:0] ID = 4'd0
) (
    input  logic       clk,
    input  logic       rst,
    output logic       ready,
    output logic [3:0] count
);
    always_ff @(posedge clk) begin
        if (rst) count <= ID;
        else count <= count + 4'd1;
    end

    assign ready = count[0];
endmodule

module tb;
    localparam int N = 6;

    logic           clk = 1'b0;
    logic           rst;
    logic [N-1:0]   ready;
    logic [4*N-1:0] counts;

    logic [3:0] high;
    logic [3:0] low;
    logic [7:0] split;

    logic [3:0] expected;
    int failures = 0;

    for (genvar i = 0; i < N; i++) begin : gen
        channel #(.ID(4'(i))) u (
            .clk  (clk),
            .rst  (rst),
            .ready(ready[i]),
            .count(counts[4*i+:4])
        );
    end

    assign split[7:4] = high;
    assign split[3:0] = low;

    // A net declared with an assignment is driven by it for the whole run.
    wire [7:0] inverted = ~split;

    always #5 clk = ~clk;

    initial begin
        rst  = 1'b1;
        high = 4'ha;
        low  = 4'h5;
        #12 rst = 1'b0;

        // Reset loaded each channel's ID at time 5; the clocks at 15 and 25 counted twice.
        #18;
        $display("t=%0d counts=%h ready=%b split=%h", $time, counts, ready, split);
        for (int i = 0; i < N; i++) begin
            expected = i + 2;
            if (counts[4*i+:4] !== expected) begin
                $display("FAIL: channel %0d counted to %h", i, counts[4*i+:4]);
                failures = failures + 1;
            end
            if (ready[i] !== expected[0]) begin
                $display("FAIL: channel %0d ready is %b", i, ready[i]);
                failures = failures + 1;
            end
        end
        if (split !== 8'ha5) begin
            $display("FAIL: split is %h", split);
            failures = failures + 1;
        end

        // Changing one half moves only that half.
        low = 4'h3;
        #1;
        $display("t=%0d split=%h inverted=%h", $time, split, inverted);
        if (split !== 8'ha3) begin
            $display("FAIL: split is %h after changing its low half", split);
            failures = failures + 1;
        end
        if (inverted !== 8'h5c) begin
            $display("FAIL: inverted is %h", inverted);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL drivers: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS drivers");
        $finish;
    end
endmodule
