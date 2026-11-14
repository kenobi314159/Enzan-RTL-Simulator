// SPDX-License-Identifier: GPL-3.0-or-later
//
// Concurrent assertions (IEEE 1800 section 16): overlapped and non-overlapped implication,
// sequences of booleans joined by fixed delays and repeated, a property with no implication, a
// named property with arguments and a named sequence, `disable iff` ending attempts in flight,
// the sampled value functions, a default clocking block, `cover` and `assume`, and an action
// block, which runs after the time slot's non-blocking assignments. Each assertion counts its
// passes and failures in its action blocks, a vacuous success among the passes, and the counts
// are checked at the end. The signals change at each falling edge from the vectors below, so
// that each rising edge samples one column of them.

module tb;
    localparam int Ticks = 16;
    localparam logic [Ticks-1:0] A   = 16'b0001_1100_1001_1010;
    localparam logic [Ticks-1:0] B   = 16'b0011_1001_1110_1100;
    localparam logic [Ticks-1:0] C   = 16'b0110_0011_0101_1000;
    localparam logic [Ticks-1:0] Rst = 16'b0000_0000_1100_0000;

    logic clk = 0, a = 0, b = 0, c = 0, rst = 0;
    int   tick_count = 0;
    int   seen = -1;
    int   errors = 0;

    always #5 clk = ~clk;
    always_ff @(posedge clk) tick_count <= tick_count + 1;

    // Column i of the vectors, the least significant first, is what the i-th rising edge samples.
    initial begin
        for (int i = 0; i < Ticks; i++) begin
            a = A[i];
            b = B[i];
            c = C[i];
            rst = Rst[i];
            @(negedge clk);
        end
    end

    int pass[12], fail[12];

    property implies_next(x, y);
        @(posedge clk) x |=> y;
    endproperty

    sequence b_then_c;
        b ##1 c;
    endsequence

    a0: assert property (@(posedge clk) a |-> b) pass[0]++; else fail[0]++;
    a1: assert property (implies_next(a, b)) pass[1]++; else fail[1]++;
    a2: assert property (@(posedge clk) a ##2 b |-> c) pass[2]++; else fail[2]++;
    a3: assert property (@(posedge clk) a |-> ##1 b_then_c) pass[3]++; else fail[3]++;
    a4: assert property (@(posedge clk) disable iff (rst) a |=> ##1 b) pass[4]++; else fail[4]++;
    a5: assert property (@(posedge clk) $rose(a) |-> b) pass[5]++; else fail[5]++;
    a6: assert property (@(posedge clk) $fell(b) |-> $stable(c)) pass[6]++; else fail[6]++;
    a7: assert property (@(posedge clk) c |-> $past(a, 2) == b) pass[7]++; else fail[7]++;
    a8: assert property (@(posedge clk) b[*3] |=> c) pass[8]++; else fail[8]++;
    a9: assert property (@(posedge clk) !(a && c)) pass[9]++; else fail[9]++;

    default clocking ticks @(posedge clk);
    endclocking

    c10: cover property (a ##1 b) pass[10]++;
    m11: assume property ($changed(a) |-> !c) pass[11]++; else fail[11]++;

    // A failing action block sees what the slot's non-blocking assignments wrote.
    a12: assert property (a |-> c) else if (seen < 0) seen = tick_count;

    initial begin
        static int expected_pass[12] = '{13, 15, 14, 14, 12, 14, 16, 13, 16, 14, 6, 13};
        static int expected_fail[12] = '{3, 1, 2, 2, 1, 2, 0, 3, 0, 2, 0, 3};
        repeat (Ticks) @(posedge clk);
        #1;
        for (int i = 0; i < 12; i++) begin
            if (pass[i] != expected_pass[i] || (i != 10 && fail[i] != expected_fail[i])) begin
                $display("FAIL assertion %0d: %0d passes and %0d failures, expected %0d and %0d",
                         i, pass[i], fail[i], expected_pass[i], expected_fail[i]);
                errors++;
            end
        end
        if (seen != 2) begin
            $display("FAIL the first failing action block saw tick_count %0d, expected 2", seen);
            errors++;
        end
        if (errors == 0) $display("PASS concurrent_assertions");
        $finish;
    end
endmodule
