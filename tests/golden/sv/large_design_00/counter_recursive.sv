// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recursive connection of counters. Allows for testing of design with deep
// hierarchy and many module instances.

module counter_recursive #(
    // Warning: Number of modules grows exponentially.
    // It is equal to INSTANCES_PER_LEVEL**LEVELS
    parameter int LEVELS               = 4
   ,parameter int INSTANCES_PER_LEVEL  = 2

   ,parameter int CNTR_WIDTH           = 8

   ,localparam OUTPUTS = int'($pow(INSTANCES_PER_LEVEL, LEVELS))
) (
    input  logic                               clk
   ,input  logic                               rst_n
   ,input  logic                               enable
   ,output logic [OUTPUTS-1:0][CNTR_WIDTH-1:0] count
);

genvar I;

localparam OUTPUTS_PER_INSTANCE = OUTPUTS / INSTANCES_PER_LEVEL;

if (LEVELS == 0) begin

    counter #(
        .WIDTH(CNTR_WIDTH)
    ) cntr_i (
        .clk(clk)
       ,.rst_n(rst_n)
       ,.enable(enable)
       ,.count(count[0])
    );

end else begin

    for (I=0; I<INSTANCES_PER_LEVEL; I++) begin : next_level_gen

        counter_recursive #(
            .LEVELS(LEVELS-1)
            ,.INSTANCES_PER_LEVEL(INSTANCES_PER_LEVEL)
            ,.CNTR_WIDTH(CNTR_WIDTH)
        ) next_level_i (
            .clk(clk)
            ,.rst_n(rst_n)
            ,.enable(enable)
            ,.count(count[I*OUTPUTS_PER_INSTANCE +: OUTPUTS_PER_INSTANCE])
        );

    end

end

endmodule
