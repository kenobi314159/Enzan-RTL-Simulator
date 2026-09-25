// SPDX-License-Identifier: GPL-3.0-or-later
//
// A parameterised synchronous counter with an asynchronous reset, driven by a self-checking
// testbench. Self-checking is strongly preferred over golden output files (DESIGN_GUIDE.md
// section 14.2): a testbench that calls $fatal on mismatch cannot drift out of date.

module counter #(
    parameter int WIDTH = 8
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             enable,
    output logic [WIDTH-1:0] count
);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            count <= '0;
        else if (enable)
            count <= count + 1'b1;
    end
endmodule

module tb;
    localparam int Width = 8;

    logic             clk = 1'b0;
    logic             rst_n = 1'b0;
    logic             enable = 1'b0;
    logic [Width-1:0] count;

    counter #(.WIDTH(Width)) u_dut (
        .clk(clk),
        .rst_n(rst_n),
        .enable(enable),
        .count(count)
    );

    always #5 clk = ~clk;

    task automatic check(input logic [Width-1:0] expected, input logic [255:0] what);
        if (count !== expected) begin
            $display("FAIL: %0s: expected %0d, got %0d at %0t", what, expected, count, $time);
            $fatal(1);
        end
    endtask

    initial begin
        // Reset holds the counter at zero.
        #12;
        if (count !== 8'd0) begin
            $display("FAIL: reset did not clear the counter");
            $fatal(1);
        end

        rst_n = 1'b1;

        // Disabled, so nothing counts even though the clock runs.
        #40;
        if (count !== 8'd0) begin
            $display("FAIL: counted while disabled");
            $fatal(1);
        end

        // Ten rising edges with the counter enabled.
        enable = 1'b1;
        #100;
        if (count !== 8'd10) begin
            $display("FAIL: expected 10 after ten edges, got %0d", count);
            $fatal(1);
        end

        // Asynchronous reset takes effect between clock edges.
        rst_n = 1'b0;
        #1;
        if (count !== 8'd0) begin
            $display("FAIL: asynchronous reset did not clear the counter");
            $fatal(1);
        end

        $display("PASS counter");
        $finish;
    end
endmodule
