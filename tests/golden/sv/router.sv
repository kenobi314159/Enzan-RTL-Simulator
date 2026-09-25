// SPDX-License-Identifier: GPL-3.0-or-later
//
// A three-level design driven through a long verification flow, as a golden case.
//
// The other cases here are each one module and a few dozen cycles. This one is deliberately
// the opposite: `tb` holds a `router`, which holds three `channel`s, each of which holds an
// `xfer_fsm` — three levels below the testbench, with the same two modules instantiated three
// times over at different parameters. It then runs four hundred and eighty transfers
// through them, some four and a half thousand cycles, and fails twenty of them on purpose.
//
// What that buys over a shorter case is the things only repetition and depth expose: a
// parameter reaching the third level with a different value per instance; an `always_ff` that
// has to keep its counters straight across hundreds of clocks; a state machine that has to
// come back to rest after being made to fail; and enough simulated time that a scheduling
// error which loses one event in a hundred has somewhere to show itself. Every check is in the
// testbench, so the case needs no golden file to drift out of date (DESIGN_GUIDE.md 14.2).

// ---------------------------------------------------------------------------
// Level 3: one transfer
// ---------------------------------------------------------------------------

/// Runs a single transfer and reports how it went.
///
/// Arming and sending are separate states so that a transfer takes a settled number of cycles
/// whatever the channel around it does, which is what lets the testbench predict the whole
/// flow's length rather than just watching for it to stop.
module xfer_fsm #(
    parameter logic [7:0] TIMEOUT = 8'd12
) (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       start,
    input  logic       ack,
    input  logic       fault,
    output logic       busy,
    output logic       sending,
    output logic       done,
    output logic       failed,
    output logic [2:0] state_out
);
    typedef enum logic [2:0] {
        S_IDLE   = 3'd0,
        S_ARM    = 3'd1,
        S_SEND   = 3'd2,
        S_WAIT   = 3'd3,
        S_DONE   = 3'd4,
        S_FAILED = 3'd5
    } state_t;

    state_t     state;
    state_t     next;
    logic [7:0] ticks;

    assign busy      = (state != S_IDLE);
    assign sending   = (state == S_SEND);
    assign done      = (state == S_DONE);
    assign failed    = (state == S_FAILED);
    assign state_out = state;

    always_comb begin
        next = state;

        case (state)
            S_IDLE: if (start) next = S_ARM;
            S_ARM: next = S_SEND;

            // A fault is noticed as the transfer leaves, before anything is waited for, which
            // is the difference between the two ways a transfer can fail.
            S_SEND: begin
                // Written out rather than as a conditional expression: a conditional over two
                // enumeration values is not itself of the enumeration's type, so assigning it
                // back needs a cast, and the cast would be noise around a plain choice.
                if (fault) next = S_FAILED;
                else next = S_WAIT;
            end

            S_WAIT: begin
                if (ack) next = S_DONE;
                else if (ticks >= TIMEOUT) next = S_FAILED;
            end

            S_DONE: next = S_IDLE;
            S_FAILED: next = S_IDLE;
            default: next = S_IDLE;
        endcase
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            ticks <= 8'd0;
        end
        else begin
            state <= next;

            // The counter belongs to the wait rather than to the machine, so it starts from
            // zero on every transfer without anything having to remember to clear it.
            if (state == S_WAIT) ticks <= ticks + 8'd1;
            else ticks <= 8'd0;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// Level 2: one channel
// ---------------------------------------------------------------------------

/// One transfer engine with its own acknowledgement timing and its own tally.
///
/// `ACK_DELAY` is what makes three instances of this module behave differently: the third
/// channel waits long enough for its acknowledgement that its transfers take nearly twice as
/// long as the first channel's, so the arbiter above sees a different length each time round.
module channel #(
    parameter logic [7:0] CHANNEL_ID = 8'd0,
    parameter logic [7:0] TIMEOUT    = 8'd12,
    parameter int         ACK_DELAY  = 2
) (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        start,
    input  logic        fault,
    input  logic        starve,
    output logic        busy,
    output logic        done,
    output logic        failed,
    output logic [2:0]  state_out,
    output logic [15:0] sent,
    output logic [15:0] lost,
    output logic [7:0]  id
);
    logic       fsm_sending;
    logic       ack;
    logic [7:0] ack_pipe;

    assign id = CHANNEL_ID;

    // The acknowledgement arrives a fixed number of cycles after the send, through a shift
    // register rather than a counter: it is the shape the thing being modelled has, and it
    // costs nothing to be honest about it.
    assign ack = ack_pipe[ACK_DELAY] & ~starve;

    xfer_fsm #(.TIMEOUT(TIMEOUT)) u_fsm (
        .clk(clk),
        .rst_n(rst_n),
        .start(start),
        .ack(ack),
        .fault(fault),
        .busy(busy),
        .sending(fsm_sending),
        .done(done),
        .failed(failed),
        .state_out(state_out)
    );

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ack_pipe <= 8'd0;
            sent     <= 16'd0;
            lost     <= 16'd0;
        end
        else begin
            ack_pipe <= {ack_pipe[6:0], fsm_sending};

            if (done) sent <= sent + 16'd1;
            if (failed) lost <= lost + 16'd1;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// Level 1: the router
// ---------------------------------------------------------------------------

/// Hands transfers to three channels in turn, one at a time, until it has issued `TOTAL`.
///
/// The arbiter is itself a small state machine — issue, then wait for the channel to report
/// back — which is what keeps `issued` and the channels' own tallies in step. A round-robin
/// that issued on a timer instead would eventually overlap a slow channel and the two counts
/// would drift apart, which is exactly the kind of thing this case exists to catch.
module router #(
    parameter logic [15:0] TOTAL = 16'd240
) (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        enable,
    input  logic        fault0,
    input  logic        fault1,
    input  logic        fault2,
    input  logic        starve0,
    input  logic        starve1,
    input  logic        starve2,
    output logic [15:0] issued,
    output logic [15:0] completed,
    output logic [15:0] failures,
    output logic [15:0] sent0,
    output logic [15:0] sent1,
    output logic [15:0] sent2,
    output logic [15:0] lost0,
    output logic [15:0] lost1,
    output logic [15:0] lost2,
    output logic [2:0]  state0,
    output logic [2:0]  state1,
    output logic [2:0]  state2,
    output logic [7:0]  id0,
    output logic [7:0]  id1,
    output logic [7:0]  id2,
    output logic        idle
);
    logic [2:0] start_vec;
    logic [1:0] grant;
    logic       waiting;

    logic ch0_busy;
    logic ch1_busy;
    logic ch2_busy;
    logic ch0_done;
    logic ch1_done;
    logic ch2_done;
    logic ch0_failed;
    logic ch1_failed;
    logic ch2_failed;

    logic start0;
    logic start1;
    logic start2;

    assign start0 = start_vec[0];
    assign start1 = start_vec[1];
    assign start2 = start_vec[2];

    // The three are the same module at three different settings: the first acknowledges
    // promptly, the third slowly enough that its transfers take nearly twice as long.
    channel #(.CHANNEL_ID(8'd10), .TIMEOUT(8'd12), .ACK_DELAY(1)) u_ch0 (
        .clk(clk),
        .rst_n(rst_n),
        .start(start0),
        .fault(fault0),
        .starve(starve0),
        .busy(ch0_busy),
        .done(ch0_done),
        .failed(ch0_failed),
        .state_out(state0),
        .sent(sent0),
        .lost(lost0),
        .id(id0)
    );

    channel #(.CHANNEL_ID(8'd11), .TIMEOUT(8'd12), .ACK_DELAY(3)) u_ch1 (
        .clk(clk),
        .rst_n(rst_n),
        .start(start1),
        .fault(fault1),
        .starve(starve1),
        .busy(ch1_busy),
        .done(ch1_done),
        .failed(ch1_failed),
        .state_out(state1),
        .sent(sent1),
        .lost(lost1),
        .id(id1)
    );

    channel #(.CHANNEL_ID(8'd12), .TIMEOUT(8'd20), .ACK_DELAY(6)) u_ch2 (
        .clk(clk),
        .rst_n(rst_n),
        .start(start2),
        .fault(fault2),
        .starve(starve2),
        .busy(ch2_busy),
        .done(ch2_done),
        .failed(ch2_failed),
        .state_out(state2),
        .sent(sent2),
        .lost(lost2),
        .id(id2)
    );

    logic any_busy;
    logic any_report;

    assign any_busy   = ch0_busy | ch1_busy | ch2_busy;
    assign any_report = ch0_done | ch1_done | ch2_done | ch0_failed | ch1_failed | ch2_failed;

    assign completed = sent0 + sent1 + sent2;
    assign failures  = lost0 + lost1 + lost2;
    assign idle      = ~any_busy & ~waiting;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            grant     <= 2'd0;
            waiting   <= 1'b0;
            issued    <= 16'd0;
            start_vec <= 3'b000;
        end
        else if (!waiting && enable && (issued < TOTAL)) begin
            case (grant)
                2'd0: start_vec <= 3'b001;
                2'd1: start_vec <= 3'b010;
                default: start_vec <= 3'b100;
            endcase

            grant   <= (grant == 2'd2) ? 2'd0 : grant + 2'd1;
            issued  <= issued + 16'd1;
            waiting <= 1'b1;
        end
        else begin
            start_vec <= 3'b000;
            if (any_report) waiting <= 1'b0;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// The testbench
// ---------------------------------------------------------------------------

module tb;
    localparam logic [15:0] Total = 16'd480;

    // How long one transfer takes, which is settled by the design rather than observed: one
    // cycle for the arbiter to issue it, one in ARM, one in SEND, the channel's own
    // acknowledgement delay in WAIT, one in DONE, and one more for the arbiter to see the
    // report.
    localparam int Channel0Cycles = 6 + 1;
    localparam int Channel1Cycles = 6 + 3;
    localparam int Channel2Cycles = 6 + 6;

    // Forty rounds of all three channels.
    localparam int CleanPhaseCycles = 40 * (Channel0Cycles + Channel1Cycles + Channel2Cycles);

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic enable = 1'b0;

    logic fault0 = 1'b0;
    logic fault1 = 1'b0;
    logic fault2 = 1'b0;
    logic starve0 = 1'b0;
    logic starve1 = 1'b0;
    logic starve2 = 1'b0;

    logic [15:0] issued;
    logic [15:0] completed;
    logic [15:0] failures;
    logic [15:0] sent0;
    logic [15:0] sent1;
    logic [15:0] sent2;
    logic [15:0] lost0;
    logic [15:0] lost1;
    logic [15:0] lost2;
    logic [2:0]  state0;
    logic [2:0]  state1;
    logic [2:0]  state2;
    logic [7:0]  id0;
    logic [7:0]  id1;
    logic [7:0]  id2;
    logic        idle;

    int failed_checks = 0;
    int guard = 0;

    router #(.TOTAL(Total)) u_sys (
        .clk(clk),
        .rst_n(rst_n),
        .enable(enable),
        .fault0(fault0),
        .fault1(fault1),
        .fault2(fault2),
        .starve0(starve0),
        .starve1(starve1),
        .starve2(starve2),
        .issued(issued),
        .completed(completed),
        .failures(failures),
        .sent0(sent0),
        .sent1(sent1),
        .sent2(sent2),
        .lost0(lost0),
        .lost1(lost1),
        .lost2(lost2),
        .state0(state0),
        .state1(state1),
        .state2(state2),
        .id0(id0),
        .id1(id1),
        .id2(id2),
        .idle(idle)
    );

    always #5 clk = ~clk;

    initial begin
        // --- reset ---------------------------------------------------------

        #12 rst_n = 1'b1;
        @(negedge clk);

        if (issued !== 16'd0 || completed !== 16'd0 || failures !== 16'd0) begin
            $display("FAIL: counters are not zero after reset");
            failed_checks = failed_checks + 1;
        end
        if (idle !== 1'b1) begin
            $display("FAIL: not idle after reset");
            failed_checks = failed_checks + 1;
        end

        // A parameter has to reach the third level with the right value per instance, which a
        // single-instance case cannot tell apart from a parameter that is ignored.
        if (id0 !== 8'd10 || id1 !== 8'd11 || id2 !== 8'd12) begin
            $display("FAIL: channel ids are %0d %0d %0d, expected 10 11 12", id0, id1, id2);
            failed_checks = failed_checks + 1;
        end

        // --- phase one: a hundred and twenty clean transfers ---------------

        enable = 1'b1;

        guard = 0;
        while (completed < 16'd120 && guard < 40000) begin
            @(negedge clk);
            guard = guard + 1;
        end

        if (failures !== 16'd0) begin
            $display("FAIL: %0d failures in the clean phase", failures);
            failed_checks = failed_checks + 1;
        end

        // Round robin over three channels, so a hundred and twenty transfers is forty each.
        // This is the check a shorter case cannot make: it is only true if every turn was
        // taken.
        if (sent0 !== 16'd40 || sent1 !== 16'd40 || sent2 !== 16'd40) begin
            $display("FAIL: uneven distribution %0d %0d %0d, expected 40 each", sent0, sent1,
                     sent2);
            failed_checks = failed_checks + 1;
        end

        // Every other check here is a count, and a count would not notice a schedule that ran
        // a cycle late. This one would. The length is fixed by the design and the reference
        // simulator agrees on it, so the differential run holds Enzan to it too.
        //
        // This is why every wait here is on the falling edge. Reading a counter on the rising
        // edge that changes it reads it before its nonblocking update lands, so the count
        // comes out one higher — and simulators do not agree about that, which would make this
        // a report about the race rather than about the schedule.
        if (guard != CleanPhaseCycles) begin
            $display("FAIL: the clean phase took %0d cycles, expected %0d", guard,
                     CleanPhaseCycles);
            failed_checks = failed_checks + 1;
        end

        $display("phase 1: %0d completed, %0d failed, %0d cycles", completed, failures, guard);

        // --- phase two: the third channel stops acknowledging --------------

        starve2 = 1'b1;

        guard = 0;
        while (lost2 < 16'd10 && guard < 20000) begin
            @(negedge clk);
            guard = guard + 1;
        end

        starve2 = 1'b0;

        if (lost0 !== 16'd0 || lost1 !== 16'd0) begin
            $display("FAIL: channels 0 and 1 lost %0d and %0d transfers while 2 was starved",
                     lost0, lost1);
            failed_checks = failed_checks + 1;
        end

        // Nothing got through on the starved channel: a timeout that fired while the
        // acknowledgement was also arriving would show up here as a transfer counted twice.
        if (sent2 !== 16'd40) begin
            $display("FAIL: channel 2 completed %0d transfers while starved, expected 40",
                     sent2);
            failed_checks = failed_checks + 1;
        end

        $display("phase 2: %0d completed, %0d failed", completed, failures);

        // --- phase three: the second channel faults immediately ------------

        fault1 = 1'b1;

        guard = 0;
        while (lost1 < 16'd10 && guard < 20000) begin
            @(negedge clk);
            guard = guard + 1;
        end

        fault1 = 1'b0;

        if (lost0 !== 16'd0) begin
            $display("FAIL: channel 0 lost %0d transfers while 1 was faulting", lost0);
            failed_checks = failed_checks + 1;
        end

        $display("phase 3: %0d completed, %0d failed", completed, failures);

        // --- phase four: run the rest of the way out -----------------------

        guard = 0;
        while ((issued < Total || !idle) && guard < 80000) begin
            @(negedge clk);
            guard = guard + 1;
        end

        if (guard >= 80000) begin
            $display("FAIL: the flow did not finish; %0d issued of %0d", issued, Total);
            $fatal(1);
        end

        // --- what the whole run has to add up to ---------------------------

        if (issued !== Total) begin
            $display("FAIL: issued %0d, expected %0d", issued, Total);
            failed_checks = failed_checks + 1;
        end

        // Every transfer either completed or failed, and none did both. An event lost anywhere
        // in several thousand cycles shows up here and nowhere else.
        if ((completed + failures) !== Total) begin
            $display("FAIL: %0d completed plus %0d failed is not %0d", completed, failures,
                     Total);
            failed_checks = failed_checks + 1;
        end

        if ((sent0 + lost0 + sent1 + lost1 + sent2 + lost2) !== Total) begin
            $display("FAIL: the channels' own tallies do not add up to %0d", Total);
            failed_checks = failed_checks + 1;
        end

        if (failures !== 16'd20) begin
            $display("FAIL: %0d failures, expected the 20 that were injected", failures);
            failed_checks = failed_checks + 1;
        end

        if (lost2 !== 16'd10 || lost1 !== 16'd10 || lost0 !== 16'd0) begin
            $display("FAIL: failures landed on the wrong channels: %0d %0d %0d", lost0, lost1,
                     lost2);
            failed_checks = failed_checks + 1;
        end

        // Round robin again: the three channels took the same number of turns over the whole
        // run, whatever happened to those turns.
        if ((sent0 + lost0) !== 16'd160 || (sent1 + lost1) !== 16'd160
            || (sent2 + lost2) !== 16'd160) begin
            $display("FAIL: turns were not shared evenly: %0d %0d %0d", sent0 + lost0,
                     sent1 + lost1, sent2 + lost2);
            failed_checks = failed_checks + 1;
        end

        // Three state machines, three levels down, all back where they started.
        if (state0 !== 3'd0 || state1 !== 3'd0 || state2 !== 3'd0) begin
            $display("FAIL: machines rest in states %0d %0d %0d, expected idle", state0, state1,
                     state2);
            failed_checks = failed_checks + 1;
        end

        if (idle !== 1'b1) begin
            $display("FAIL: the router is not idle at the end");
            failed_checks = failed_checks + 1;
        end

        // --- and it must stay stopped --------------------------------------

        repeat (50) @(negedge clk);

        if (issued !== Total || (completed + failures) !== Total) begin
            $display("FAIL: the router kept going after reaching its target");
            failed_checks = failed_checks + 1;
        end

        $display("router: %0d transfers, %0d completed, %0d failed", issued, completed,
                 failures);

        // --- the reset is asynchronous -------------------------------------
        //
        // Every `always_ff` above is sensitive to the reset as well as to the clock, and
        // nothing so far can tell that apart from a synchronous reset: the reset was released
        // between edges and the design was at rest by the first one that followed. Dropping it
        // in the middle of a low half of the clock, and looking before the next rising edge,
        // can — the counters have to be clear already.

        @(negedge clk);
        #1 rst_n = 1'b0;
        #1;

        if (issued !== 16'd0 || completed !== 16'd0 || failures !== 16'd0) begin
            $display("FAIL: reset did not clear the counters until the next clock edge");
            failed_checks = failed_checks + 1;
        end
        if (state0 !== 3'd0 || state1 !== 3'd0 || state2 !== 3'd0) begin
            $display("FAIL: reset did not return the machines to idle until the next clock edge");
            failed_checks = failed_checks + 1;
        end

        // And it has to start again from there rather than merely stop, which is what the
        // whole flow running a second time says.

        @(negedge clk);
        #1 rst_n = 1'b1;

        guard = 0;
        while (completed < 16'd30 && guard < 20000) begin
            @(negedge clk);
            guard = guard + 1;
        end

        if (sent0 !== 16'd10 || sent1 !== 16'd10 || sent2 !== 16'd10 || failures !== 16'd0) begin
            $display("FAIL: after reset the flow restarted as %0d %0d %0d with %0d failures",
                     sent0, sent1, sent2, failures);
            failed_checks = failed_checks + 1;
        end

        $display("after reset: %0d completed, %0d failed", completed, failures);

        if (failed_checks != 0) begin
            $display("FAIL router: %0d checks failed", failed_checks);
            $fatal(1);
        end

        $display("PASS router");
        $finish;
    end
endmodule
