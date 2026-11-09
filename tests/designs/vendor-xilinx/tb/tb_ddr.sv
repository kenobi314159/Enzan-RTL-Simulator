// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's 7-series DDR registers. ODDR sends D1 while the clock is high and D2 while it is low, in
// OPPOSITE_EDGE mode taking D2 at the falling edge and in SAME_EDGE mode at the rising one. IDDR
// takes D at both edges: in OPPOSITE_EDGE mode Q1 at the rising edge and Q2 at the falling one, and
// in SAME_EDGE_PIPELINED mode both at the rising edge, aligned. Each value is checked settled,
// between edges.

`timescale 1ns / 1ps

module tb_ddr;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic d1 = 1'b0, d2 = 1'b0;
    wire q_opposite, q_same;
    ODDR #(.DDR_CLK_EDGE("OPPOSITE_EDGE")) u_oddr_opposite (.Q(q_opposite), .C(clk), .CE(1'b1),
                                                           .D1(d1), .D2(d2), .R(1'b0), .S(1'b0));
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) u_oddr_same (.Q(q_same), .C(clk), .CE(1'b1), .D1(d1),
                                                   .D2(d2), .R(1'b0), .S(1'b0));

    logic d = 1'b0;
    wire q1_opposite, q2_opposite, q1_pipelined, q2_pipelined;
    IDDR #(.DDR_CLK_EDGE("OPPOSITE_EDGE")) u_iddr_opposite (.Q1(q1_opposite), .Q2(q2_opposite),
                                                           .C(clk), .CE(1'b1), .D(d), .R(1'b0),
                                                           .S(1'b0));
    IDDR #(.DDR_CLK_EDGE("SAME_EDGE_PIPELINED")) u_iddr_pipelined (.Q1(q1_pipelined),
                                                                 .Q2(q2_pipelined), .C(clk),
                                                                 .CE(1'b1), .D(d), .R(1'b0),
                                                                 .S(1'b0));

    int failures = 0;
    initial begin
        #200;
        for (int step = 0; step < 8; step++) begin
            // New data a little after the falling edge, for the rising edge that follows.
            @(negedge clk);
            #1 {d1, d2} = 2'(step * 3 + 1);
            d = step[0];
            @(posedge clk);
            #1 d = !step[0];
            #2 $display("step %0d high: opposite=%b same=%b", step, q_opposite, q_same);
            if (q_opposite !== d1 || q_same !== d1) failures = failures + 1;
            @(negedge clk);
            #2 $display("step %0d low: opposite=%b same=%b iddr %b%b %b%b", step, q_opposite,
                        q_same, q1_opposite, q2_opposite, q1_pipelined, q2_pipelined);
            if (q_opposite !== d2 || q_same !== d2) failures = failures + 1;
            if (q1_opposite !== step[0] || q2_opposite !== !step[0]) failures = failures + 1;
            // Pipelined: the rising edge's value and the falling edge's before it, which the
            // stimulus makes the same.
            if (step > 0 && (q1_pipelined !== step[0] || q2_pipelined !== step[0]))
                failures = failures + 1;
        end
        if (failures == 0) $display("PASS tb_ddr");
        $finish;
    end
endmodule
