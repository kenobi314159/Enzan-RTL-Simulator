// SPDX-License-Identifier: GPL-3.0-or-later
//
// Streaming concatenations (IEEE 1800 section 11.4.14): `>>` and `<<` with the default slice, a
// number of bits and a type as the slice, a slice that does not divide the stream, operands of
// several widths and an unpacked array among them, a stream assigned to a wider target and as a
// target itself, nested, and in a continuous assignment. Each expected value is worked by the
// standard's rule: the stream is the operands packed, the first most significant; `<<` reverses
// its slices, counted from the least significant end, so that a short one is the stream's leftmost
// bits and ends last; a wider target is filled from the left, and a target takes the value's
// leftmost bits and puts its slices back. slang folds the localparams by the same rules.

module tb;
    logic [7:0]  b;
    logic [15:0] h;
    logic [31:0] w;
    logic [5:0]  six;
    logic [3:0]  n1, n2;
    logic [7:0]  bytes [0:3];
    logic [31:0] reversed_bits;
    logic [31:0] swapped_bytes;
    logic [5:0]  r6;
    logic [7:0]  r8;
    logic [15:0] r16;
    logic [23:0] r24;
    int          errors = 0;

    localparam logic [5:0]  Folded6 = {<<4{6'b11_0101}};
    localparam logic [31:0] FoldedWide = {<<{8'b1100_1010}};

    assign reversed_bits = {<<{w}};
    assign swapped_bytes = {<<byte{w}};

    // Each check is numbered, and its comment says what it checks.
    task automatic check(input logic [31:0] got, input logic [31:0] expected, input int number);
        if (got !== expected) begin
            $display("FAIL check %0d: got %h, expected %h", number, got, expected);
            errors++;
        end
    endtask

    initial begin
        b = 8'b1100_1010;
        r8 = {<<{b}};
        check(r8, 8'b0101_0011, 1);  // << reverses the bits
        r8 = {>>{b}};
        check(r8, 8'b1100_1010, 2);  // >> keeps the order

        h = 16'hABCD;
        r16 = {<<4{h}};
        check(r16, 16'hDCBA, 3);  // << 4 reverses the nibbles
        r16 = {<<byte{h}};
        check(r16, 16'hCDAB, 4);  // << byte swaps the bytes
        r24 = {<<8{h, b}};
        check(r24, 24'hCA_CD_AB, 5);  // << 8 over two operands
        r24 = {>>{h, b}};
        check(r24, 24'hAB_CD_CA, 6);  // >> over two operands

        // `{<< 4 {6'b11_0101}}` is the standard's own example.
        six = 6'b11_0101;
        r6 = {<<4{six}};
        check(r6, 6'b0101_11, 7);  // << 4 of six bits leaves a short slice
        r6 = {<<3{six}};
        check(r6, 6'b101_110, 8);  // << 3 of six bits

        check(Folded6, 6'b0101_11, 9);  // slang's own folding of the standard's example
        check(FoldedWide, 32'h5300_0000, 10);  // and of a stream left-justified

        // A stream into a wider target is left-justified and filled with zeros on the right.
        w = {<<{b}};
        check(w, 32'h5300_0000, 11);  // a stream left-justified in a wider target
        w = {>>{h}};
        check(w, 32'hABCD_0000, 12);  // a >> stream left-justified

        // An unpacked array streams its elements, the first the most significant.
        bytes = '{8'h11, 8'h22, 8'h33, 8'h44};
        w = {>>{bytes}};
        check(w, 32'h1122_3344, 13);  // an unpacked array streamed >>
        w = {<<byte{bytes}};
        check(w, 32'h4433_2211, 14);  // an unpacked array streamed << byte

        // As a target: the value's bits from its most significant end fill the operands.
        {>>{n1, n2}} = 8'hA5;
        check({n1, n2}, 8'hA5, 15);  // >> target over two operands
        {<<4{n1, n2}} = 8'hA5;
        check({n1, n2}, 8'h5A, 16);  // << 4 target swaps the nibbles
        {<<{b}} = 8'b1110_0000;
        check(b, 8'b0000_0111, 17);  // << target reverses the bits
        {>>{n1}} = 16'hBEEF;
        check(n1, 4'hB, 18);  // a target narrower than its value takes the top bits
        {>>{bytes}} = 32'hDEAD_BEEF;
        check({bytes[0], bytes[1], bytes[2], bytes[3]}, 32'hDEAD_BEEF, 19);  // an unpacked array target

        // A stream inside a stream orders its own slices.
        h = 16'h1234;
        r16 = {<<8{{<<4{h}}}};
        check(r16, 16'h2143, 20);  // a stream of a stream

        // A continuous assignment of a stream follows what it reads.
        w = 32'h0000_0001;
        #1;
        check(reversed_bits, 32'h8000_0000, 21);  // a continuous << of bits
        check(swapped_bytes, 32'h0100_0000, 22);  // a continuous << byte
        w = 32'h1234_5678;
        #1;
        check(swapped_bytes, 32'h7856_3412, 23);  // a continuous << byte after a change

        if (errors == 0) $display("PASS streaming");
        $finish;
    end
endmodule
