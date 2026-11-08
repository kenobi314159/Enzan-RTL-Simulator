// SPDX-License-Identifier: GPL-3.0-or-later
//
// Event controls on expressions rather than signals (IEEE 1800 section 9.4.2), each of which
// resumes when the expression's value changes or, with an edge, when its value makes the edge: an
// element chosen by a variable, as Altera's multiply-accumulate models wait on
// `@(result_pipe[head] or head)`; an edge of a logical expression, as its PLL models wait on
// one; and a sum whose operands change together without changing it, which resumes nothing.
// Each starts with its operands initialised, which is no change at time 0.
//
// The case checks itself. Icarus Verilog resumes on any change of an operand, the sum's
// included, and Verilator at time 0 as well; every other line is as both print it.

module tb;
    reg [7:0] pipe [0:3];
    reg [1:0] head = 2'd0;
    reg a = 1'b0, b = 1'b0;
    reg [7:0] xy = 8'h34;
    integer element_wakes = 0, edge_wakes = 0, sum_wakes = 0;

    always @(pipe[head] or head) begin
        element_wakes = element_wakes + 1;
        $display("t=%0t element: head=%0d pipe[head]=%h", $time, head, pipe[head]);
    end

    always @(posedge (a && b)) begin
        edge_wakes = edge_wakes + 1;
        $display("t=%0t edge: a=%b b=%b", $time, a, b);
    end

    always @(xy[7:4] + xy[3:0]) begin
        sum_wakes = sum_wakes + 1;
        $display("t=%0t sum: xy=%h", $time, xy);
    end

    initial begin
        #1 begin
            pipe[0] = 8'h10;
            pipe[1] = 8'h11;
            pipe[2] = 8'h12;
            pipe[3] = 8'h13;
        end
        #10 pipe[0] = 8'h20;  // the element chosen: resumes
        #10 pipe[1] = 8'h21;  // another: does not
        #10 head = 2'd1;  // the choice, and the element with it: resumes
        #10 a = 1'b1;  // a && b stays 0
        #10 b = 1'b1;  // rises: resumes
        #10 a = 1'b0;  // falls: does not
        #10 xy = 8'h43;  // the sum stays 7
        #10 xy = 8'h53;  // 8: resumes
        #10;
        $display("element=%0d edge=%0d sum=%0d", element_wakes, edge_wakes, sum_wakes);
        if (element_wakes == 3 && edge_wakes == 1 && sum_wakes == 1) $display("PASS event_expressions");
        else $display("FAIL: event_expressions");
        $finish;
    end
endmodule
