// SPDX-License-Identifier: GPL-3.0-or-later
//
// The time-zero and region-ordering rules of IEEE 1800.
//
// **This case is deliberately not compared against a reference simulator.** Two correct
// simulators may legitimately differ in delta-level ordering, and time zero is where that
// bites hardest: Icarus Verilog does not evaluate `always_comb` at time zero, so it
// reports X here where IEEE 1800 section 9.2.2.2.1 says the process is triggered once at
// time zero. Enzan follows the standard, and pinning that with a differential comparison
// would only encode another tool's behaviour.

module tb;
    int failures = 0;

    logic [7:0] comb_in = 8'd10;
    logic [7:0] comb_out;
    logic       clk = 1'b0;
    logic [7:0] seq_out = 8'd0;
    logic [7:0] a = 8'h11;
    logic [7:0] b = 8'h22;
    logic [7:0] blocking_probe;

    always_comb comb_out = comb_in + 8'd1;

    always_ff @(posedge clk) seq_out <= seq_out + 8'd2;

    initial begin
        // `always_comb` is triggered once at time zero, so a combinational output is
        // consistent before any input has moved.
        if (comb_out !== 8'd11) begin
            $display("FAIL: always_comb should evaluate at time zero, got %0d", comb_out);
            failures = failures + 1;
        end

        // `always_ff` is not: a flip-flop whose clock has not edged has done nothing, and
        // running its body would load a value without a clock ever having arrived.
        if (seq_out !== 8'd0) begin
            $display("FAIL: always_ff should not run before a clock edge, got %0d", seq_out);
            failures = failures + 1;
        end

        // Non-blocking assignments read every right-hand side before applying any update, so
        // this swaps rather than copying one value into both.
        a <= b;
        b <= a;
        #1;
        if (a !== 8'h22 || b !== 8'h11) begin
            $display("FAIL: non-blocking assignments should swap, got %h and %h", a, b);
            failures = failures + 1;
        end

        // A blocking assignment is visible immediately within the same process.
        a = 8'h33;
        blocking_probe = a;
        if (blocking_probe !== 8'h33) begin
            $display("FAIL: a blocking assignment should be visible at once, got %h",
                     blocking_probe);
            failures = failures + 1;
        end

        // A zero delay resumes in this timestamp but after everything already scheduled, so
        // time has not moved.
        #0;
        if ($time !== 1) begin
            $display("FAIL: #0 should not advance time, now %0t", $time);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL scheduling: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS scheduling");
        $finish;
    end
endmodule
