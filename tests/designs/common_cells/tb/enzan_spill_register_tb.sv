// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks cc_spill_register, registered and bypassed, carrying a packed struct numbered in
// order. The source offers a new item at random and holds it until it is taken. The sink is
// ready at random. Everything the source sends must arrive in order, none lost and none twice.
// The registered form must also take an item while its output is stalled, as long as it has
// room, which is what a spill register is for.

module enzan_spill_check #(
    parameter bit Bypass = 1'b0
) (
    output logic done,
    output int   failures
);
    localparam int unsigned Items = 1500;

    typedef struct packed {
        logic [15:0] sequence_number;
        logic [7:0]  check;
    } item_t;

    logic  clk = 1'b0;
    logic  rst_n = 1'b0;
    logic  valid_in = 1'b0;
    logic  ready_out;
    item_t data_in;
    logic  valid_out;
    logic  ready_in = 1'b0;
    item_t data_out;

    cc_spill_register #(.data_t(item_t), .Bypass(Bypass)) u_spill (
        .clk_i   (clk),
        .rst_ni  (rst_n),
        .clr_i   (1'b0),
        .valid_i (valid_in),
        .ready_o (ready_out),
        .data_i  (data_in),
        .valid_o (valid_out),
        .ready_i (ready_in),
        .data_o  (data_out)
    );

    always #5 clk = ~clk;

    int unsigned state = Bypass ? 32'h00c0_ffee : 32'hfeed_0001;
    int unsigned sent = 0;
    int unsigned received = 0;
    int unsigned held = 0;
    int unsigned taken_while_stalled = 0;

    function automatic int unsigned next(int unsigned value);
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value;
    endfunction

    function automatic logic [7:0] check_of(logic [15:0] sequence_number);
        return sequence_number[7:0] ^ sequence_number[15:8] ^ 8'h5a;
    endfunction

    // Inputs change only at the falling edge, and what a rising edge does is worked out from
    // values sampled just before it, so nothing here races the flip-flops it drives.
    initial begin
        logic accepted;
        logic delivered;

        done = 1'b0;
        failures = 0;
        accepted = 1'b0;
        repeat (2) @(negedge clk);
        rst_n = 1'b1;

        while (received < Items) begin
            @(negedge clk);
            if (accepted) valid_in = 1'b0;
            state = next(state);
            // An offer is held until it is taken, as the handshake requires.
            if (!valid_in && sent < Items && state[0]) begin
                valid_in = 1'b1;
                data_in = item_t'{sequence_number: 16'(sent), check: check_of(16'(sent))};
            end
            ready_in = state[1] | state[2];

            #4;
            if (Bypass && (valid_out !== valid_in || ready_out !== ready_in
                           || (valid_in && data_out !== data_in))) begin
                $display("FAIL bypass: valid %b->%b, ready %b<-%b", valid_in, valid_out,
                         ready_out, ready_in);
                failures++;
            end
            if (valid_out) begin
                if (data_out.sequence_number != 16'(received)
                    || data_out.check !== check_of(data_out.sequence_number)) begin
                    $display("FAIL bypass=%b: got item %0d with check %h, expected item %0d",
                             Bypass, data_out.sequence_number, data_out.check, received);
                    failures++;
                end
            end
            if (!Bypass && valid_in && ready_out && valid_out && !ready_in)
                taken_while_stalled++;
            accepted = valid_in && ready_out;
            delivered = valid_out && ready_in;

            @(posedge clk);
            if (delivered) received++;
            if (accepted) sent++;
            if (sent < received || sent - received > (Bypass ? 0 : 2)) begin
                $display("FAIL bypass=%b: %0d sent and %0d received", Bypass, sent, received);
                failures++;
            end
        end

        if (!Bypass && taken_while_stalled == 0) begin
            $display("FAIL: the spill register never took an item while its output was stalled");
            failures++;
        end
        done = 1'b1;
    end
endmodule

module enzan_spill_register_tb;
    logic registered_done;
    logic bypass_done;
    int   registered_failures;
    int   bypass_failures;

    enzan_spill_check #(.Bypass(1'b0)) u_registered (
        .done(registered_done), .failures(registered_failures)
    );
    enzan_spill_check #(.Bypass(1'b1)) u_bypass (
        .done(bypass_done), .failures(bypass_failures)
    );

    initial begin
        wait (registered_done === 1'b1 && bypass_done === 1'b1);
        if (registered_failures + bypass_failures != 0) begin
            $display("FAIL enzan_spill_register_tb: %0d checks failed",
                     registered_failures + bypass_failures);
            $fatal(1);
        end
        $display("PASS enzan_spill_register_tb");
        $finish;
    end
endmodule
