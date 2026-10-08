// SPDX-License-Identifier: GPL-3.0-or-later
//
// Checks cc_lzc in both modes, chosen by an enum parameter from cc_pkg, at a width that is not a
// power of two and at width 1, its special case. Every input is compared against a count made
// bit by bit. An empty input reports `empty_o` with a count of Width - 1.

module enzan_lzc_tb;
    import cc_pkg::*;

    localparam int unsigned Width = 11;
    localparam int unsigned CntWidth = idx_width(Width);

    logic [Width-1:0]    in;
    logic [CntWidth-1:0] leading;
    logic [CntWidth-1:0] trailing;
    logic                leading_empty;
    logic                trailing_empty;
    logic                bit_in;
    logic                bit_count;
    logic                bit_empty;

    cc_lzc #(.Width(Width), .Mode(LZC_LEADING_ZERO_CNT)) u_leading (
        .in_i(in), .cnt_o(leading), .empty_o(leading_empty)
    );
    cc_lzc #(.Width(Width), .Mode(LZC_TRAILING_ZERO_CNT)) u_trailing (
        .in_i(in), .cnt_o(trailing), .empty_o(trailing_empty)
    );
    cc_lzc #(.Width(1), .Mode(LZC_TRAILING_ZERO_CNT)) u_single (
        .in_i(bit_in), .cnt_o(bit_count), .empty_o(bit_empty)
    );

    int failures = 0;

    function automatic int unsigned count_leading(logic [Width-1:0] value);
        for (int bit_index = Width - 1; bit_index >= 0; bit_index--)
            if (value[bit_index]) return Width - 1 - bit_index;
        return Width - 1;
    endfunction

    function automatic int unsigned count_trailing(logic [Width-1:0] value);
        for (int bit_index = 0; bit_index < Width; bit_index++)
            if (value[bit_index]) return bit_index;
        return Width - 1;
    endfunction

    initial begin
        if (CntWidth != 4) begin
            $display("FAIL: idx_width(%0d) should be 4, got %0d", Width, CntWidth);
            failures++;
        end

        for (int value = 0; value < 2 ** Width; value++) begin
            in = value;
            #1;
            if (leading != count_leading(in) || trailing != count_trailing(in)
                || leading_empty !== (value == 0) || trailing_empty !== (value == 0)) begin
                $display("FAIL: %b gave leading %0d, trailing %0d, empty %b and %b", in, leading,
                         trailing, leading_empty, trailing_empty);
                failures++;
            end
        end

        bit_in = 1'b0;
        #1 if (bit_count !== 1'b1 || bit_empty !== 1'b1) begin
            $display("FAIL: width 1, input 0 gave count %b, empty %b", bit_count, bit_empty);
            failures++;
        end
        bit_in = 1'b1;
        #1 if (bit_count !== 1'b0 || bit_empty !== 1'b0) begin
            $display("FAIL: width 1, input 1 gave count %b, empty %b", bit_count, bit_empty);
            failures++;
        end

        if (failures != 0) begin
            $display("FAIL enzan_lzc_tb: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS enzan_lzc_tb: %0d inputs", 2 ** Width);
        $finish;
    end
endmodule
