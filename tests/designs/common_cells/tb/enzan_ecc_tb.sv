// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks cc_ecc_encode and cc_ecc_decode, the SECDED code whose widths and code word layout come
// from functions and a packed struct in cc_pkg. Every word goes through unharmed, with one bit
// flipped, and with two flipped. One flip must be corrected and reported as a single error, or
// as a parity error when it is the overall parity bit. Two must be reported as a double error.

module enzan_ecc_tb;
    import cc_pkg::*;

    localparam int unsigned DataWidth = 32;
    localparam int unsigned CodeWidth = ecc_get_cw_width(DataWidth);
    localparam int unsigned EncodedWidth = CodeWidth + 1;
    localparam int unsigned Words = 300;

    typedef logic [DataWidth-1:0] data_t;
    typedef struct packed {
        logic                 parity;
        logic [CodeWidth-1:0] code_word;
    } encoded_t;

    data_t    data;
    encoded_t encoded;
    encoded_t received;
    data_t    decoded;
    logic [ecc_get_parity_width(DataWidth)-1:0] syndrome;
    logic     single_error;
    logic     parity_error;
    logic     double_error;

    cc_ecc_encode #(.DataWidth(DataWidth)) u_encode (.data_i(data), .data_o(encoded));
    cc_ecc_decode #(.DataWidth(DataWidth)) u_decode (
        .data_i         (received),
        .data_o         (decoded),
        .syndrome_o     (syndrome),
        .single_error_o (single_error),
        .parity_error_o (parity_error),
        .double_error_o (double_error)
    );

    int unsigned state = 32'h1234_5678;
    int failures = 0;

    function automatic int unsigned next(int unsigned value);
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        return value;
    endfunction

    task automatic expect_word(input logic [8*9-1:0] what, input logic want_single,
                               input logic want_parity, input logic want_double);
        #1;
        if (!want_double && decoded !== data) begin
            $display("FAIL %s: %h decoded as %h", what, data, decoded);
            failures++;
        end
        if (single_error !== want_single || parity_error !== want_parity
            || double_error !== want_double) begin
            $display("FAIL %s: %h gave single=%b parity=%b double=%b", what, data, single_error,
                     parity_error, double_error);
            failures++;
        end
    endtask

    initial begin
        int unsigned first;
        int unsigned second;

        if ($bits(encoded_t) != EncodedWidth || CodeWidth != 38) begin
            $display("FAIL: a %0d-bit word should code to 38 bits and a parity bit, got %0d",
                     DataWidth, $bits(encoded_t));
            failures++;
        end

        for (int word = 0; word < Words; word++) begin
            state = next(state);
            data = state;
            #1;

            received = encoded;
            expect_word("clean", 1'b0, 1'b0, 1'b0);

            first = next(state) % EncodedWidth;
            received = encoded;
            received[first] = ~received[first];
            // The struct's first field, the overall parity bit, is its most significant.
            expect_word("one flip", first != EncodedWidth - 1, first == EncodedWidth - 1, 1'b0);

            second = (first + 1 + next(next(state)) % (EncodedWidth - 1)) % EncodedWidth;
            received[second] = ~received[second];
            expect_word("two flips", 1'b0, 1'b0, 1'b1);
        end

        if (failures != 0) begin
            $display("FAIL enzan_ecc_tb: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS enzan_ecc_tb: %0d words", Words);
        $finish;
    end
endmodule
