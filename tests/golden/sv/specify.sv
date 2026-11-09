// SPDX-License-Identifier: GPL-3.0-or-later
//
// Specify blocks as cell libraries write them (IEEE 1800 sections 30 and 31). A specparam is a
// constant that expressions use like a parameter. A module path whose delays are all zero changes
// nothing, whatever its form, and a timing check is not run, so the flop below, written as vendor
// models are, behaves as its `always` block says, with its notifier never toggling.

module flop (
    input      clk,
    input      d,
    input      en,
    output reg q,
    output     qn
);
    specparam setup_time = 2, hold_time = 1;
    reg notifier = 1'b0;

    always @(posedge clk) if (en) q <= d;
    assign qn = ~q;

    specify
        specparam PATHPULSE$ = 0;
        specparam tpd = 0;
        (posedge clk => (q +: d)) = (0:0:0, 0:0:0);
        if (en) (clk => qn) = tpd;
        ifnone (clk => qn) = (0, 0);
        (d, en *> q) = 0;
        $setuphold(posedge clk, d, setup_time, hold_time, notifier);
        $width(posedge clk, 3, 0, notifier);
        $period(posedge clk, 10);
    endspecify
endmodule

module tb;
    specparam half_period = 5;
    logic clk = 1'b0;
    logic d = 1'b0;
    logic en = 1'b1;
    wire  q, qn;
    int   failures = 0;

    flop u_flop (.clk(clk), .d(d), .en(en), .q(q), .qn(qn));

    always #half_period clk = ~clk;

    task automatic expect_q(logic want);
        $display("t=%0d d=%b en=%b q=%b qn=%b", $time, d, en, q, qn);
        if (q !== want || qn !== ~want) begin
            $display("FAIL: q should be %b", want);
            failures = failures + 1;
        end
    endtask

    initial begin
        if (half_period != 5 || u_flop.setup_time != 2) begin
            $display("FAIL: a specparam should be a constant");
            failures = failures + 1;
        end

        // Changed one unit before an edge, inside the setup time the specify block asks for: the
        // flop takes the value it sees, and nothing marks the violation.
        @(negedge clk) #4 d = 1'b1;
        @(posedge clk) #1 expect_q(1'b1);
        @(negedge clk) begin
            d = 1'b0;
            en = 1'b0;
        end
        if (u_flop.notifier !== 1'b0) begin
            $display("FAIL: a notifier should never toggle");
            failures = failures + 1;
        end
        @(negedge clk) begin
            expect_q(1'b1);
            en = 1'b1;
        end
        @(negedge clk) expect_q(1'b0);

        if (failures == 0) $display("PASS specify");
        $finish;
    end
endmodule
