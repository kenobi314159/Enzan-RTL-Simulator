// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exercises the operator set against values a human can check by eye, including the
// X-propagation rules of IEEE 1800. Anything here that disagrees with a reference
// simulator is a semantic defect, not a style difference.

module tb;
    int failures = 0;
    localparam int NONE = 0;

    task automatic expect_eq8(input logic [7:0] got, input logic [7:0] want,
                              input int line_number);
        if (got !== want) begin
            $display("FAIL line %0d: got %b, expected %b", line_number, got, want);
            failures = failures + 1;
        end
    endtask

    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] r;
    logic [3:0] narrow;
    logic       bit_result;
    logic [3:0][7:0] bytes;

    initial begin
        // --- arithmetic ---------------------------------------------------
        a = 8'd200;
        b = 8'd100;
        r = a + b;
        if (r !== 8'd44) begin
            $display("FAIL: 200 + 100 should wrap to 44, got %0d", r);
            failures = failures + 1;
        end

        r = b - a;
        if (r !== 8'd156) begin
            $display("FAIL: 100 - 200 should wrap to 156, got %0d", r);
            failures = failures + 1;
        end

        a = 8'd12;
        r = a * a;
        if (r !== 8'd144) begin
            $display("FAIL: 12 * 12 should be 144, got %0d", r);
            failures = failures + 1;
        end

        a = 8'd100;
        b = 8'd7;
        if ((a / b) !== 8'd14 || (a % b) !== 8'd2) begin
            $display("FAIL: 100/7 and 100%%7 should be 14 and 2, got %0d and %0d", a / b, a % b);
            failures = failures + 1;
        end

        // Division by zero is X, not a trap and not zero.
        b = 8'd0;
        if ((a / b) !== 8'bxxxxxxxx) begin
            $display("FAIL: division by zero should be all x, got %b", a / b);
            failures = failures + 1;
        end

        // --- X propagation ------------------------------------------------
        // Zero dominates in AND, one dominates in OR: getting this wrong is the classic
        // source of spurious X through a gated clock.
        a = 8'b0000_1111;
        b = 8'bxxxx_xxxx;
        if ((a & b) !== 8'b0000_xxxx) begin
            $display("FAIL: 0f & xx should be 0000xxxx, got %b", a & b);
            failures = failures + 1;
        end
        if ((a | b) !== 8'bxxxx_1111) begin
            $display("FAIL: 0f | xx should be xxxx1111, got %b", a | b);
            failures = failures + 1;
        end

        // One unknown bit poisons a whole arithmetic result.
        a = 8'b0000_000x;
        b = 8'd1;
        if ((a + b) !== 8'bxxxxxxxx) begin
            $display("FAIL: adding an unknown bit should give all x, got %b", a + b);
            failures = failures + 1;
        end

        // `==` is unknown when either side is; `===` never is.
        a = 8'b0000_000x;
        bit_result = (a == 8'd1);
        if (bit_result !== 1'bx) begin
            $display("FAIL: == with an unknown operand should be x, got %b", bit_result);
            failures = failures + 1;
        end
        bit_result = (a === 8'b0000_000x);
        if (bit_result !== 1'b1) begin
            $display("FAIL: === should match exactly, got %b", bit_result);
            failures = failures + 1;
        end

        // --- shifts -------------------------------------------------------
        a = 8'b1000_0001;
        if ((a << 1) !== 8'b0000_0010) begin
            $display("FAIL: shift left, got %b", a << 1);
            failures = failures + 1;
        end
        if ((a >> 1) !== 8'b0100_0000) begin
            $display("FAIL: shift right, got %b", a >> 1);
            failures = failures + 1;
        end
        if ((a << 8) !== 8'b0000_0000) begin
            $display("FAIL: shifting by the full width should clear it, got %b", a << 8);
            failures = failures + 1;
        end

        // --- reductions and logical ops -----------------------------------
        a = 8'b1111_1111;
        if ((&a) !== 1'b1 || (|a) !== 1'b1 || (^a) !== 1'b0) begin
            $display("FAIL: reductions of all-ones");
            failures = failures + 1;
        end
        a = 8'b0000_0001;
        if ((&a) !== 1'b0 || (|a) !== 1'b1 || (^a) !== 1'b1) begin
            $display("FAIL: reductions of one bit set");
            failures = failures + 1;
        end

        // A vector holding a set bit is true even with unknown bits present.
        a = 8'b0000_001x;
        if ((a ? 1'b1 : 1'b0) !== 1'b1) begin
            $display("FAIL: a set bit should decide the condition");
            failures = failures + 1;
        end

        // An unknown condition merges the arms, keeping the bits they agree on.
        bit_result = 1'bx;
        r = bit_result ? 8'b1100_0000 : 8'b1010_0000;
        if (r !== 8'b1xx0_0000) begin
            $display("FAIL: unknown condition should merge per bit, got %b", r);
            failures = failures + 1;
        end

        // --- selects, concatenation and replication ------------------------
        a = 8'b1101_0010;
        if (a[0] !== 1'b0 || a[1] !== 1'b1 || a[7] !== 1'b1) begin
            $display("FAIL: bit selects of %b", a);
            failures = failures + 1;
        end
        if (a[5:2] !== 4'b0100) begin
            $display("FAIL: part select, got %b", a[5:2]);
            failures = failures + 1;
        end
        if (a[2 +: 4] !== 4'b0100) begin
            $display("FAIL: indexed-up part select, got %b", a[2 +: 4]);
            failures = failures + 1;
        end
        if (a[5 -: 4] !== 4'b0100) begin
            $display("FAIL: indexed-down part select, got %b", a[5 -: 4]);
            failures = failures + 1;
        end

        // An out-of-range select is x, which the language defines rather than erroring.
        narrow = 4'b1010;
        if (narrow[7] !== 1'bx) begin
            $display("FAIL: out-of-range select should be x, got %b", narrow[7]);
            failures = failures + 1;
        end

        narrow = 4'b1010;
        r = {narrow, 4'b0101};
        if (r !== 8'b1010_0101) begin
            $display("FAIL: concatenation, got %b", r);
            failures = failures + 1;
        end
        // A replication counted to zero adds nothing to the concatenation around it.
        r = {narrow, {NONE{1'b1}}, 4'b0101};
        if (r !== 8'b1010_0101) begin
            $display("FAIL: a zero-count replication, got %b", r);
            failures = failures + 1;
        end
        r = {2{4'b1100}};
        if (r !== 8'b1100_1100) begin
            $display("FAIL: replication, got %b", r);
            failures = failures + 1;
        end

        // Writing through a part select leaves the rest alone.
        r = 8'b0000_0000;
        r[5:2] = 4'b1111;
        if (r !== 8'b0011_1100) begin
            $display("FAIL: part-select write, got %b", r);
            failures = failures + 1;
        end

        // Writing through a concatenation splits the value.
        {narrow, r[3:0]} = 8'b1001_0110;
        if (narrow !== 4'b1001 || r[3:0] !== 4'b0110) begin
            $display("FAIL: concatenation write, got %b and %b", narrow, r[3:0]);
            failures = failures + 1;
        end

        // The bit-counting functions count bits that are exactly 1, and see X and Z for what
        // they are.
        a = 8'b1010_0x1z;
        if ($isunknown(a) !== 1'b1 || $countones(a) != 3 || $onehot(a) !== 1'b0
            || $onehot0(8'b0) !== 1'b1 || $onehot(8'b0001_0000) !== 1'b1) begin
            $display("FAIL: $isunknown, $countones or $onehot of %b", a);
            failures = failures + 1;
        end

        // `$clog2` and `$bits` of something that is not a constant, as in a loop bound: the
        // logarithm rounds up and is 0 for 0, and `$bits` is the width without evaluating.
        a = 8'd5;
        b = 8'd128;
        if ($clog2(a) != 3 || $clog2(b) != 7 || $clog2(a - 8'd5) != 0 || $bits(a + b) != 8) begin
            $display("FAIL: $clog2 of %0d or %0d, or $bits of a sum", a, b);
            failures = failures + 1;
        end

        // `**` wraps at the width, and an unknown operand makes it x.
        a = 8'd3;
        narrow = 4'd4;
        if ((a ** narrow) !== 8'd81 || (a ** 4'bx000) !== 8'bxxxx_xxxx) begin
            $display("FAIL: 3 ** 4 should be 81, got %0d", a ** narrow);
            failures = failures + 1;
        end

        // A bit known on both sides and different settles `==` whatever the unknown bits hold,
        // where a relation they could decide is x (IEEE 1800 section 11.4.5).
        narrow = 4'b1x00;
        if ((narrow == 4'b0000) !== 1'b0 || (narrow != 4'b0000) !== 1'b1
            || (narrow == 4'b1000) !== 1'bx) begin
            $display("FAIL: == with an unknown bit, got %b %b %b", narrow == 4'b0000,
                     narrow != 4'b0000, narrow == 4'b1000);
            failures = failures + 1;
        end

        // A case item matches by ===, so an item with x or z matches a selector holding the same.
        narrow = 4'b10x1;
        bit_result = 1'b0;
        case (narrow)
            4'b1001: bit_result = 1'b0;
            4'b10x1: bit_result = 1'b1;
            default: bit_result = 1'b0;
        endcase
        if (bit_result !== 1'b1) begin
            $display("FAIL: a case item with x did not match a selector holding it");
            failures = failures + 1;
        end

        // A literal's z is z, not x: `===` tells them apart.
        narrow = 4'bz01x;
        if (narrow === 4'bx01x || narrow !== 4'bz01x) begin
            $display("FAIL: a z in a literal, got %b", narrow);
            failures = failures + 1;
        end

        // A constant part select of a packed array counts in elements, not bits: [2:1] of
        // four bytes is the middle two bytes.
        bytes = 32'h4433_2211;
        if (bytes[2:1] !== 16'h3322) begin
            $display("FAIL: packed-array part select, got %h", bytes[2:1]);
            failures = failures + 1;
        end
        bytes[2:1] = 16'hbbaa;
        if (bytes !== 32'h44bb_aa11) begin
            $display("FAIL: packed-array part-select write, got %h", bytes);
            failures = failures + 1;
        end

        // --- signedness ---------------------------------------------------
        if ($signed(4'b1111) >= 0 || $unsigned(-4'sd1) != 4'd15) begin
            $display("FAIL: $signed and $unsigned");
            failures = failures + 1;
        end

        if ($signed(8'hFF) >= 0) begin
            $display("FAIL: 8'hff should be negative when signed");
            failures = failures + 1;
        end
        if (8'hFF <= 0) begin
            $display("FAIL: 8'hff should be positive when unsigned");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL operators: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS operators");
        $finish;
    end
endmodule
