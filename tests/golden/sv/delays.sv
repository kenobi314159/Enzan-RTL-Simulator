// SPDX-License-Identifier: GPL-3.0-or-later
//
// Assignment delays (IEEE 1800 sections 9.4.5 and 10.3.3). An intra-assignment delay evaluates
// the right-hand side when the statement runs and assigns it later: a blocking one waits in
// between, and a nonblocking one carries on and schedules the update. A delay on a continuous
// assignment is inertial: a change that arrives before the previous one has taken effect
// replaces it, so a pulse shorter than the delay never reaches the target.

module tb;
    logic [7:0] source = 8'd0;
    logic [7:0] blocking_copy;
    logic [7:0] nonblocking_copy;
    logic [7:0] queued;
    logic       pulse = 1'b0;
    wire        delayed_pulse;
    wire  [7:0] delayed_source;
    int failures = 0;

    assign #3 delayed_pulse = pulse;
    assign #2 delayed_source = source;

    // Two updates in flight at once on one variable: a transport delay keeps both.
    initial begin
        queued = 8'd0;
        queued <= #4 8'd1;
        queued <= #6 8'd2;
    end

    initial begin
        #1 source = 8'd10;
        blocking_copy = #2 source + 8'd1;
        $display("t=%0d blocking_copy=%0d", $time, blocking_copy);
        if ($time != 3 || blocking_copy !== 8'd11) begin
            $display("FAIL: blocking intra-assignment delay");
            failures = failures + 1;
        end

        nonblocking_copy <= #2 source;
        source = 8'd20;
        #1 if (nonblocking_copy !== 8'bx) begin
            $display("FAIL: a nonblocking update arrived early");
            failures = failures + 1;
        end
        // It lands in the NBA region of time 5, after this process has looked at time 5.
        #2 $display("t=%0d nonblocking_copy=%0d", $time, nonblocking_copy);
        if (nonblocking_copy !== 8'd10) begin
            $display("FAIL: nonblocking intra-assignment delay gave %0d", nonblocking_copy);
            failures = failures + 1;
        end
        if (queued !== 8'd1) begin
            $display("FAIL: the first queued update should have landed, queued=%0d", queued);
            failures = failures + 1;
        end

        // A pulse of 1 against an inertial delay of 3 is swallowed; one of 4 gets through.
        pulse = 1'b1;
        #1 pulse = 1'b0;
        #5 if (delayed_pulse !== 1'b0) begin
            $display("FAIL: a pulse shorter than the delay got through");
            failures = failures + 1;
        end
        pulse = 1'b1;
        #4 pulse = 1'b0;
        if (delayed_pulse !== 1'b1) begin
            $display("FAIL: a pulse longer than the delay did not get through");
            failures = failures + 1;
        end
        #4 $display("t=%0d queued=%0d delayed_source=%0d delayed_pulse=%b", $time, queued,
                    delayed_source, delayed_pulse);
        if (queued !== 8'd2 || delayed_source !== 8'd20 || delayed_pulse !== 1'b0) begin
            $display("FAIL: final values");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL delays: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS delays");
        $finish;
    end
endmodule
