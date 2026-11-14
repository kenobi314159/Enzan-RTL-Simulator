// SPDX-License-Identifier: GPL-3.0-or-later
//
// The counters of tests/golden/sv/large_design_00, as many and as deep as its testbench makes
// them, counting for a fixed number of cycles: sixty-four at the bottom of a hierarchy six
// generate levels deep, and sixty more side by side.
//
// Where pipeline.sv is a few processes doing some work each, this is many processes doing a
// little: a hundred and twenty-four woken on every edge, and their values carried up through
// every level of ports to the top. The cycle count is fixed in the source for the reason
// pipeline.sv gives.

module bench #(
    parameter int CYCLES = 25000
);
    localparam int Width = 18;
    localparam int Levels = 6;
    localparam int PerLevel = 2;
    localparam int Linear = 60;
    localparam int Recursive = PerLevel ** Levels;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    int   cycle_count = 0;

    logic [Recursive-1:0][Width-1:0] count_rec;
    logic [Linear-1:0][Width-1:0]    count_lin;

    counter_recursive #(
        .LEVELS(Levels),
        .INSTANCES_PER_LEVEL(PerLevel),
        .CNTR_WIDTH(Width)
    ) cntr_rec_i (
        .clk(clk),
        .rst_n(rst_n),
        .enable(1'b1),
        .count(count_rec)
    );

    for (genvar i = 0; i < Linear; i++) begin : cntr_lin_gen
        counter #(.WIDTH(Width)) cntr_i (
            .clk(clk),
            .rst_n(rst_n),
            .enable(1'b1),
            .count(count_lin[i])
        );
    end

    always #5 clk = ~clk;

    always_ff @(posedge clk) begin
        cycle_count <= cycle_count + 1;
        if (cycle_count == 2) rst_n <= 1'b1;
    end

    initial begin
        while (cycle_count < CYCLES)
            @(posedge clk);

        // Printed so that the harness can confirm the run did the work it was asked for, and
        // so that a change which accidentally optimises the design away is visible.
        $display("cycles=%0d first=%0d last=%0d", cycle_count, count_rec[0],
                 count_lin[Linear-1]);
        $finish;
    end
endmodule
