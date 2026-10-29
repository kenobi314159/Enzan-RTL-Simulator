// SPDX-License-Identifier: GPL-3.0-or-later
//
// A synthetic benchmark: a chain of registered stages with combinational logic between them,
// clocked for a fixed number of cycles.
//
// Synthetic rather than a real design, deliberately. A benchmark is a *fixed* design with *fixed*
// stimulus, because a benchmark whose input changes measures nothing across commits. Real designs
// are fetched under tests/designs/ as capability grows and become benchmarks once they simulate;
// until then a synthetic pipeline exercises what dominates a real run — the clock edge, the
// non-blocking update queue, and combinational settling — with a cycle count that is exactly what
// the harness asked for. The cycle count is fixed in the source rather than overridden per run,
// because a benchmark whose input varies between runs is not a benchmark.
//
// The stages are written out rather than generated over a packed array, and stay so: a
// benchmark whose source changes measures nothing across commits.

module stage #(
    parameter int WIDTH = 32
) (
    input  logic             clk,
    input  logic [WIDTH-1:0] d,
    output logic [WIDTH-1:0] q
);
    logic [WIDTH-1:0] mixed;

    // Enough combinational work per stage that the benchmark is not purely scheduler
    // overhead, but nothing beyond the commonest constructs.
    always_comb mixed = (d ^ (d >> 3)) + 32'h9E37_79B9;

    always_ff @(posedge clk) q <= mixed;
endmodule

module bench #(
    parameter int CYCLES = 100000,
    parameter int WIDTH  = 32
);
    logic             clk = 1'b0;
    logic [WIDTH-1:0] seed = 32'h1234_5678;
    int               cycle_count = 0;

    logic [WIDTH-1:0] s0;
    logic [WIDTH-1:0] s1;
    logic [WIDTH-1:0] s2;
    logic [WIDTH-1:0] s3;
    logic [WIDTH-1:0] s4;
    logic [WIDTH-1:0] s5;
    logic [WIDTH-1:0] s6;
    logic [WIDTH-1:0] s7;

    stage #(.WIDTH(WIDTH)) u_stage0 (.clk(clk), .d(seed), .q(s0));
    stage #(.WIDTH(WIDTH)) u_stage1 (.clk(clk), .d(s0), .q(s1));
    stage #(.WIDTH(WIDTH)) u_stage2 (.clk(clk), .d(s1), .q(s2));
    stage #(.WIDTH(WIDTH)) u_stage3 (.clk(clk), .d(s2), .q(s3));
    stage #(.WIDTH(WIDTH)) u_stage4 (.clk(clk), .d(s3), .q(s4));
    stage #(.WIDTH(WIDTH)) u_stage5 (.clk(clk), .d(s4), .q(s5));
    stage #(.WIDTH(WIDTH)) u_stage6 (.clk(clk), .d(s5), .q(s6));
    stage #(.WIDTH(WIDTH)) u_stage7 (.clk(clk), .d(s6), .q(s7));

    always #5 clk = ~clk;

    always_ff @(posedge clk) begin
        cycle_count <= cycle_count + 1;
        seed <= seed + 32'd1;
    end

    initial begin
        while (cycle_count < CYCLES)
            @(posedge clk);

        // Printed so that the harness can confirm the run did the work it was asked for, and
        // so that a change which accidentally optimises the design away is visible.
        $display("cycles=%0d final=%0h", cycle_count, s7);
        $finish;
    end
endmodule
