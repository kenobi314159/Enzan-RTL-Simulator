// SPDX-License-Identifier: GPL-3.0-or-later
//
// if/else, case, and the loop forms, plus the combinational and sequential process kinds and
// the scheduling rules that distinguish them.

module tb;
    int failures = 0;

    logic [7:0] sum;
    logic [3:0] selector;
    logic [7:0] decoded;
    logic [7:0] comb_in = 8'd0;
    logic [7:0] comb_out;
    logic       clk = 1'b0;
    logic [7:0] seq_out = 8'd0;

    // always_comb evaluates once at time zero, so its output is consistent before any input
    // has moved.
    always_comb comb_out = comb_in + 8'd1;

    // always_ff does not, because a flip-flop whose clock has not edged has done nothing.
    always_ff @(posedge clk) seq_out <= seq_out + 8'd2;

    always_comb begin
        case (selector)
            4'd0:       decoded = 8'h01;
            4'd1:       decoded = 8'h02;
            4'd2, 4'd3: decoded = 8'h04;
            default:    decoded = 8'hFF;
        endcase
    end

    initial begin
        // --- if / else ----------------------------------------------------
        sum = 8'd0;
        if (sum == 8'd0)
            sum = 8'd10;
        else
            sum = 8'd20;
        if (sum !== 8'd10) begin
            $display("FAIL: if took the wrong branch, got %0d", sum);
            failures = failures + 1;
        end

        // An unknown condition takes the else branch.
        sum = 8'd0;
        if (1'bx)
            sum = 8'd1;
        else
            sum = 8'd2;
        if (sum !== 8'd2) begin
            $display("FAIL: an unknown condition should take the else branch, got %0d", sum);
            failures = failures + 1;
        end

        // --- case ---------------------------------------------------------
        selector = 4'd0;
        #1;
        if (decoded !== 8'h01) begin
            $display("FAIL: case item 0, got %h", decoded);
            failures = failures + 1;
        end
        selector = 4'd3;
        #1;
        if (decoded !== 8'h04) begin
            $display("FAIL: a grouped case item, got %h", decoded);
            failures = failures + 1;
        end
        selector = 4'd9;
        #1;
        if (decoded !== 8'hFF) begin
            $display("FAIL: case default, got %h", decoded);
            failures = failures + 1;
        end

        // --- for ----------------------------------------------------------
        sum = 8'd0;
        for (int i = 0; i < 5; i = i + 1)
            sum = sum + 8'd3;
        if (sum !== 8'd15) begin
            $display("FAIL: for loop, got %0d", sum);
            failures = failures + 1;
        end

        // A loop whose condition is false from the start runs zero times.
        sum = 8'd7;
        for (int i = 0; i < 0; i = i + 1)
            sum = 8'd0;
        if (sum !== 8'd7) begin
            $display("FAIL: a loop with a false condition should not run");
            failures = failures + 1;
        end

        // A nested loop starts its variable again on every pass of the loop around it.
        sum = 8'd0;
        for (int i = 0; i < 3; i = i + 1)
            for (int j = 0; j < 4; j = j + 1)
                sum = sum + 8'd1;
        if (sum !== 8'd12) begin
            $display("FAIL: a nested loop ran %0d times instead of 12", sum);
            failures = failures + 1;
        end

        // --- while and repeat ---------------------------------------------
        sum = 8'd0;
        while (sum < 8'd9)
            sum = sum + 8'd2;
        if (sum !== 8'd10) begin
            $display("FAIL: while loop, got %0d", sum);
            failures = failures + 1;
        end

        sum = 8'd0;
        repeat (4)
            sum = sum + 8'd5;
        if (sum !== 8'd20) begin
            $display("FAIL: repeat loop, got %0d", sum);
            failures = failures + 1;
        end

        // The count of a repeat is evaluated once, before the loop.
        sum = 8'd0;
        repeat (0)
            sum = 8'd99;
        if (sum !== 8'd0) begin
            $display("FAIL: repeat 0 should not run");
            failures = failures + 1;
        end

        // --- increment operators ------------------------------------------
        sum = 8'd5;
        sum++;
        if (sum !== 8'd6) begin
            $display("FAIL: postincrement, got %0d", sum);
            failures = failures + 1;
        end
        sum--;
        if (sum !== 8'd5) begin
            $display("FAIL: postdecrement, got %0d", sum);
            failures = failures + 1;
        end
        sum += 8'd4;
        if (sum !== 8'd9) begin
            $display("FAIL: compound add, got %0d", sum);
            failures = failures + 1;
        end

        // --- combinational settling and a clock edge -----------------------
        comb_in = 8'd41;
        #1;
        if (comb_out !== 8'd42) begin
            $display("FAIL: always_comb did not settle, got %0d", comb_out);
            failures = failures + 1;
        end

        clk = 1'b1;
        #1;
        if (seq_out !== 8'd2) begin
            $display("FAIL: always_ff did not fire on the edge, got %0d", seq_out);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL control_flow: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS control_flow");
        $finish;
    end
endmodule
