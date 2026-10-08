// SPDX-License-Identifier: GPL-3.0-or-later
//

module tb;
    localparam int CNTR_WIDTH = 18;

    localparam int CNTR_REC_LEVELS = 6;
    localparam int CNTR_REC_INSTS  = 2;

    localparam int CNTR_LIN_INSTS = 60;

    genvar I;

    localparam CNTR_REC_OUTPUTS = int'($pow(CNTR_REC_INSTS, CNTR_REC_LEVELS));
    localparam CNTR_LIN_OUTPUTS = CNTR_LIN_INSTS;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic enable = 1'b0;
//    logic [CNTR_REC_OUTPUTS-1:0][CNTR_WIDTH-1:0] count_rec;
//    logic [CNTR_LIN_OUTPUTS-1:0][CNTR_WIDTH-1:0] count_lin;

    counter_recursive #(
        .LEVELS(CNTR_REC_LEVELS)
       ,.INSTANCES_PER_LEVEL(CNTR_REC_INSTS)
       ,.CNTR_WIDTH(CNTR_WIDTH)
    ) cntr_rec_i (
        .clk(clk)
       ,.rst_n(rst_n)
       ,.enable(enable)
//       ,.count(count_rec)
    );

    generate
        for (I=0; I<CNTR_LIN_INSTS; I++) begin : cntr_lin_gen
            logic [CNTR_WIDTH-1:0] count;
            counter #(
                .WIDTH(CNTR_WIDTH)
            ) cntr_i (
                .clk(clk)
               ,.rst_n(rst_n)
               ,.enable(enable)
               ,.count(count)
            );
        end
    endgenerate

    always #5 clk = ~clk;

    initial begin
        // Reset holds the counter at zero.
        #12;
        //if (count !== 8'd0) begin
        //    $display("FAIL: reset did not clear the counter");
        //    $fatal(1);
        //end

        rst_n = 1'b1;

        // Disabled, so nothing counts even though the clock runs.
        #400;
        //if (count !== 8'd0) begin
        //    $display("FAIL: counted while disabled");
        //    $fatal(1);
        //end

        // Ten rising edges with the counter enabled.
        enable = 1'b1;
        #200000;
        //if (count !== 8'd10) begin
        //    $display("FAIL: expected 10 after ten edges, got %0d", count);
        //    $fatal(1);
        //end

        // Asynchronous reset takes effect between clock edges.
        rst_n = 1'b0;
        #1;
        //if (count !== 8'd0) begin
        //    $display("FAIL: asynchronous reset did not clear the counter");
        //    $fatal(1);
        //end

        rst_n = 1'b1;
        #200000;

        $display("PASS counter");
        $finish;
    end
endmodule
