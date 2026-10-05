// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks cc_fifo against a model, carrying a packed struct, at a depth that is not a power of two
// and in both modes: registered, and fall-through, where a push into an empty FIFO is visible
// at the output in the same cycle. Pushes and pops are random and legal: no push when full, no
// pop when empty. Now and then a flush empties the FIFO.

module enzan_fifo_check #(
    parameter bit FallThrough = 1'b0
) (
    output logic done,
    output int   failures
);
    localparam int unsigned Depth = 5;
    localparam int unsigned Cycles = 2000;

    typedef struct packed {
        logic [3:0]  tag;
        logic [11:0] value;
        logic        flag;
    } item_t;

    logic  clk = 1'b0;
    logic  rst_n = 1'b0;
    logic  flush = 1'b0;
    logic  full;
    logic  empty;
    logic [cc_pkg::cnt_width(Depth)-1:0] usage;
    item_t data_in;
    item_t data_out;
    logic  push = 1'b0;
    logic  pop = 1'b0;

    cc_fifo #(.FallThrough(FallThrough), .Depth(Depth), .data_t(item_t)) u_fifo (
        .clk_i   (clk),
        .rst_ni  (rst_n),
        .clr_i   (1'b0),
        .flush_i (flush),
        .full_o  (full),
        .empty_o (empty),
        .usage_o (usage),
        .data_i  (data_in),
        .push_i  (push),
        .data_o  (data_out),
        .pop_i   (pop)
    );

    always #5 clk = ~clk;

    item_t model [Depth];
    int unsigned head = 0;
    int unsigned count = 0;
    int unsigned state = FallThrough ? 32'h0bad_cafe : 32'h1357_9bdf;

    function automatic int unsigned next(int unsigned value);
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value;
    endfunction

    initial begin
        logic passing;
        item_t expected;

        done = 1'b0;
        failures = 0;
        repeat (2) @(negedge clk);
        rst_n = 1'b1;

        for (int cycle = 0; cycle < Cycles; cycle++) begin
            // Inputs change away from the clock edge, and are checked just before the next.
            @(negedge clk);
            state = next(state);
            flush = state[31:28] == 4'd0;
            push = state[0] && count < Depth;
            pop = state[1] && (count > 0 || (FallThrough && push));
            data_in = item_t'(state[27:11]);

            #4;
            passing = FallThrough && count == 0 && push;
            if (full !== (count == Depth) || empty !== (count == 0 && !passing)
                || usage != count) begin
                $display("FAIL fall-through=%b cycle %0d: full=%b empty=%b usage=%0d, model %0d",
                         FallThrough, cycle, full, empty, usage, count);
                failures++;
            end
            if (passing || count > 0) begin
                expected = passing ? data_in : model[head];
                if (data_out !== expected) begin
                    $display("FAIL fall-through=%b cycle %0d: data %h, expected %h", FallThrough,
                             cycle, data_out, expected);
                    failures++;
                end
            end

            @(posedge clk);
            if (flush) begin
                head = 0;
                count = 0;
            end else if (!(passing && pop)) begin
                if (push) begin
                    model[(head + count) % Depth] = data_in;
                    count++;
                end
                if (pop) begin
                    head = (head + 1) % Depth;
                    count--;
                end
            end
        end
        done = 1'b1;
    end
endmodule

module enzan_fifo_tb;
    logic registered_done;
    logic through_done;
    int   registered_failures;
    int   through_failures;

    enzan_fifo_check #(.FallThrough(1'b0)) u_registered (
        .done(registered_done), .failures(registered_failures)
    );
    enzan_fifo_check #(.FallThrough(1'b1)) u_through (
        .done(through_done), .failures(through_failures)
    );

    initial begin
        wait (registered_done === 1'b1 && through_done === 1'b1);
        if (registered_failures + through_failures != 0) begin
            $display("FAIL enzan_fifo_tb: %0d checks failed",
                     registered_failures + through_failures);
            $fatal(1);
        end
        $display("PASS enzan_fifo_tb");
        $finish;
    end
endmodule
