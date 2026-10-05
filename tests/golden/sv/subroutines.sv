// SPDX-License-Identifier: GPL-3.0-or-later
//
// Functions and tasks, which the frontend inlines into the process that calls them: locals,
// loops and early returns in a function; a function called from a continuous assignment and
// from always_comb, whose sensitivity reaches what the function reads; a function reading a
// module variable; output and inout arguments; a task with delays, which suspend its caller;
// and calls nested in calls.

module tb;
    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] parity_source;
    logic [7:0] doubled;
    logic       parity;
    logic [7:0] scale;
    logic [7:0] scaled;
    logic       clk = 1'b0;
    int failures = 0;

    function automatic logic [7:0] add_saturating(input logic [7:0] x, input logic [7:0] y);
        logic [8:0] sum;
        sum = x + y;
        if (sum[8]) return 8'hff;
        return sum[7:0];
    endfunction

    function automatic logic [3:0] count_ones(input logic [7:0] value);
        logic [3:0] count;
        count = 0;
        for (int i = 0; i < 8; i++) count = count + value[i];
        return count;
    endfunction

    function automatic logic odd(input logic [7:0] value);
        odd = count_ones(value) % 2 == 1;
    endfunction

    // Reads a module variable, so whatever calls it is sensitive to that variable.
    function automatic logic [7:0] times_scale(input logic [7:0] value);
        return value * scale;
    endfunction

    task automatic split(input logic [7:0] value, output logic [3:0] high, output logic [3:0] low);
        high = value[7:4];
        low = value[3:0];
    endtask

    task automatic accumulate(inout logic [7:0] total, input logic [7:0] amount);
        total = total + amount;
    endtask

    task automatic pulse(input int cycles);
        repeat (cycles) begin
            #5 clk = 1'b1;
            #5 clk = 1'b0;
        end
    endtask

    assign doubled = add_saturating(a, a);
    always_comb parity = odd(parity_source);
    always_comb scaled = times_scale(b);

    initial begin
        logic [3:0] high;
        logic [3:0] low;
        logic [7:0] total;
        time started;

        a = 8'd100;
        parity_source = 8'b1011_0001;
        scale = 8'd3;
        b = 8'd5;
        #1;
        $display("doubled=%0d parity=%b scaled=%0d", doubled, parity, scaled);
        if (doubled !== 8'd200 || parity !== 1'b0 || scaled !== 8'd15) begin
            $display("FAIL: function results at the start");
            failures = failures + 1;
        end

        a = 8'd200;
        parity_source = 8'b1011_0011;
        scale = 8'd4;
        #1;
        $display("doubled=%0d parity=%b scaled=%0d", doubled, parity, scaled);
        if (doubled !== 8'hff || parity !== 1'b1 || scaled !== 8'd20) begin
            $display("FAIL: function results after their inputs changed");
            failures = failures + 1;
        end

        split(8'ha5, high, low);
        total = 8'd10;
        accumulate(total, 8'd7);
        accumulate(total, add_saturating(8'd1, 8'd2));
        $display("high=%h low=%h total=%0d", high, low, total);
        if (high !== 4'ha || low !== 4'h5 || total !== 8'd20) begin
            $display("FAIL: output and inout arguments");
            failures = failures + 1;
        end

        started = $time;
        pulse(3);
        $display("pulse took %0d", $time - started);
        if ($time - started != 30) begin
            $display("FAIL: a task's delays should suspend its caller for 30, took %0d",
                     $time - started);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL subroutines: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS subroutines");
        $finish;
    end
endmodule
