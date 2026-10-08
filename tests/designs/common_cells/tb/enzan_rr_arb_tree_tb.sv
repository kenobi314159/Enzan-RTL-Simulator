// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks cc_rr_arb_tree with six inputs, a number that leaves its tree unbalanced, carrying a
// packed struct that names its source. A request stays up until it is granted, and the
// downstream grant is random. Each cycle the arbiter must forward a requesting input's data
// with its index, and grant that input alone when downstream grants. With fair arbitration a
// waiting input must be served before any other is served six times.

module enzan_rr_arb_tree_tb;
    localparam int unsigned NumIn = 6;
    localparam int unsigned Cycles = 3000;

    typedef struct packed {
        logic [2:0] source;
        logic [7:0] payload;
    } item_t;

    logic                          clk = 1'b0;
    logic                          rst_n = 1'b0;
    logic [NumIn-1:0]              req = '0;
    logic [NumIn-1:0]              gnt;
    item_t [NumIn-1:0]             data;
    logic                          req_out;
    logic                          gnt_in = 1'b0;
    item_t                         data_out;
    logic [$clog2(NumIn)-1:0]      idx;

    cc_rr_arb_tree #(.NumIn(NumIn), .data_t(item_t), .FairArb(1'b1)) u_arbiter (
        .clk_i  (clk),
        .rst_ni (rst_n),
        .clr_i  (1'b0),
        .rr_i   ('0),
        .req_i  (req),
        .gnt_o  (gnt),
        .data_i (data),
        .req_o  (req_out),
        .gnt_i  (gnt_in),
        .data_o (data_out),
        .idx_o  (idx)
    );

    always #5 clk = ~clk;

    int unsigned state = 32'h2468_ace1;
    int unsigned others_served [NumIn];
    int unsigned grants [NumIn];
    int failures = 0;

    function automatic int unsigned next(int unsigned value);
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value;
    endfunction

    // Inputs change only at the falling edge, and what a rising edge does is worked out from
    // values sampled just before it, so nothing here races the flip-flops it drives.
    initial begin
        logic                     granted;
        logic [$clog2(NumIn)-1:0] granted_index;
        logic [NumIn-1:0]         waiting;

        granted = 1'b0;
        for (int source = 0; source < NumIn; source++) begin
            data[source] = item_t'{source: 3'(source), payload: 8'(source * 37)};
            others_served[source] = 0;
            grants[source] = 0;
        end
        repeat (2) @(negedge clk);
        rst_n = 1'b1;

        for (int cycle = 0; cycle < Cycles; cycle++) begin
            @(negedge clk);
            if (granted) req[granted_index] = 1'b0;
            state = next(state);
            // New requests join the ones still waiting; a waiting one is never withdrawn.
            req = req | NumIn'(state);
            gnt_in = state[8] | state[9];
            for (int source = 0; source < NumIn; source++)
                if (!req[source]) data[source].payload = 8'(state >> (source + 10));

            #4;
            if (req_out !== |req) begin
                $display("FAIL cycle %0d: req_o=%b for requests %b", cycle, req_out, req);
                failures++;
            end
            if (req_out) begin
                if (idx >= NumIn || !req[idx] || data_out !== data[idx]
                    || data_out.source != idx) begin
                    $display("FAIL cycle %0d: index %0d with data %h for requests %b", cycle, idx,
                             data_out, req);
                    failures++;
                end
                if (gnt !== (gnt_in ? NumIn'(1) << idx : '0)) begin
                    $display("FAIL cycle %0d: grants %b for index %0d, downstream %b", cycle, gnt,
                             idx, gnt_in);
                    failures++;
                end
            end else if (gnt !== '0) begin
                $display("FAIL cycle %0d: grants %b with nothing requesting", cycle, gnt);
                failures++;
            end

            granted = req_out && gnt_in;
            granted_index = idx;
            waiting = req;

            @(posedge clk);
            if (granted) begin
                grants[granted_index]++;
                for (int source = 0; source < NumIn; source++) begin
                    if (source == granted_index) begin
                        others_served[source] = 0;
                    end else if (waiting[source] && ++others_served[source] >= NumIn) begin
                        $display("FAIL cycle %0d: input %0d starved", cycle, source);
                        failures++;
                    end
                end
            end
        end

        for (int source = 0; source < NumIn; source++) begin
            if (grants[source] == 0) begin
                $display("FAIL: input %0d was never granted", source);
                failures++;
            end
        end

        if (failures != 0) begin
            $display("FAIL enzan_rr_arb_tree_tb: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS enzan_rr_arb_tree_tb");
        $finish;
    end
endmodule
