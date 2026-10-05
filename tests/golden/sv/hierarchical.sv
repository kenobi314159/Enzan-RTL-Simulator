// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hierarchical references: a testbench reading and writing a DUT's internals, through a
// generate block and two levels down; a module reading its parent's signal upwards; and one
// instance reading its sibling's. Two instances share one definition, so a path that held for
// only one of them would show here as the other reading the wrong instance.

module counter #(parameter logic [7:0] STEP = 8'd1) (input logic clk);
    logic [7:0] count = 8'd0;
    always_ff @(posedge clk) count <= count + STEP;

    // Upwards, to the testbench that holds every counter.
    logic [7:0] seen_limit;
    assign seen_limit = tb.limit;
endmodule

module pair (input logic clk);
    counter #(.STEP(8'd1)) first (.clk(clk));
    counter #(.STEP(8'd1)) second (.clk(clk));

    // Across, from this level into a child's internals.
    logic [7:0] first_count;
    assign first_count = first.count;
endmodule

module tb;
    logic       clk = 1'b0;
    logic [7:0] limit = 8'd42;
    int failures = 0;

    pair u_pair (.clk(clk));

    for (genvar i = 0; i < 2; i++) begin : gen
        counter #(.STEP(8'd5)) u (.clk(clk));
    end

    always #5 clk = ~clk;

    initial begin
        #31;
        $display("first=%0d second=%0d gen0=%0d gen1=%0d", u_pair.first.count,
                 u_pair.second.count, gen[0].u.count, gen[1].u.count);
        if (u_pair.first.count !== 8'd3 || gen[1].u.count !== 8'd15) begin
            $display("FAIL: reading a counter's count from above");
            failures = failures + 1;
        end
        if (u_pair.first_count !== u_pair.first.count) begin
            $display("FAIL: a parent reading its child's internals");
            failures = failures + 1;
        end
        if (gen[0].u.seen_limit !== 8'd42 || u_pair.second.seen_limit !== 8'd42) begin
            $display("FAIL: reading the testbench from below");
            failures = failures + 1;
        end

        // Writing through a reference reaches the instance's own storage.
        u_pair.second.count = 8'd100;
        #10;
        $display("second=%0d", u_pair.second.count);
        if (u_pair.second.count !== 8'd101 || u_pair.first.count !== 8'd4) begin
            $display("FAIL: writing a counter's count from above");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL hierarchical: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS hierarchical");
        $finish;
    end
endmodule
