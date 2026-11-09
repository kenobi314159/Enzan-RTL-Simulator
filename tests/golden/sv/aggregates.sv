// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unpacked arrays and structs where Icarus Verilog cannot be the reference: whole-array copy and
// equality, and assignment patterns, positional, replicated or `'{default: ...}`, as an
// initialiser, a statement and a continuous assignment, none of which Icarus Verilog 12
// implements; an array through a port, which it leaves X; an index past an inner dimension, which
// it lets land in the next row where IEEE 1800 section 7.4.6 makes the read the type's default and
// the write nothing; and structs, packed and unpacked, alone and in an array. The case checks
// itself against what the standard gives.

module reverse (
    input  logic [7:0] in  [0:3],
    output logic [7:0] out [0:3]
);
    assign out[0] = in[3];
    assign out[1] = in[2];
    assign out[2] = in[1];
    assign out[3] = in[0];
endmodule

module tb;
    typedef struct packed {
        logic [3:0] high;
        logic [3:0] low;
    } pair_t;

    typedef struct {
        logic [7:0] tag;
        int         count;
        bit   [3:0] flags;
    } record_t;

    logic [7:0] memory [0:15];
    logic [7:0] copy [0:15];
    logic [7:0] grid [0:2][0:3];
    logic [7:0] filled [0:3];
    logic [7:0] preset [0:3] = '{8'h11, 8'h22, 8'h33, 8'h44};
    wire  [7:0] swapped [0:1];
    logic [7:0] reversed [0:3];
    logic [0:7] ascending;
    pair_t      pair;
    record_t    record;
    record_t    records [0:3];
    logic [3:0] address;
    int failures = 0;

    assign swapped = '{preset[1], preset[0]};
    reverse u_reverse (.in(preset), .out(reversed));

    initial begin
        for (int i = 0; i < 16; i++) memory[i] = 8'(i * 3);
        copy = memory;
        if (copy != memory || copy[7] !== 8'd21) begin
            $display("FAIL: whole-array copy");
            failures = failures + 1;
        end
        copy[3] = 8'h00;
        if (copy == memory) begin
            $display("FAIL: arrays that differ in one element compared equal");
            failures = failures + 1;
        end

        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) grid[r][c] = 8'(r * 16 + c);
        address = 4'd5;
        if (grid[1][address] !== 8'bxxxx_xxxx) begin
            $display("FAIL: an index past the inner dimension read %b", grid[1][address]);
            failures = failures + 1;
        end
        grid[0][address] = 8'hff;
        if (grid[1][1] !== 8'h11) begin
            $display("FAIL: a write past the inner dimension landed in the next row");
            failures = failures + 1;
        end

        filled = '{default: 8'h5a};
        filled[2] = 8'h01;
        if (filled[0] !== 8'h5a || filled[2] !== 8'h01 || filled[3] !== 8'h5a) begin
            $display("FAIL: default pattern");
            failures = failures + 1;
        end

        // Positional patterns are in order, left element first.
        if (preset[0] !== 8'h11 || preset[3] !== 8'h44) begin
            $display("FAIL: initialiser gave %h and %h", preset[0], preset[3]);
            failures = failures + 1;
        end
        filled = '{8'h01, 8'h02, 8'h03, 8'h04};
        #1;
        if (filled[0] !== 8'h01 || filled[3] !== 8'h04 || swapped[0] !== 8'h22
            || swapped[1] !== 8'h11 || reversed[0] !== 8'h44 || reversed[3] !== 8'h11) begin
            $display("FAIL: positional pattern, one continuously assigned, or an array port");
            failures = failures + 1;
        end

        // A replicated pattern repeats its elements, in order.
        filled = '{2{8'h12, preset[2]}};
        if (filled[0] !== 8'h12 || filled[1] !== 8'h33 || filled[2] !== 8'h12
            || filled[3] !== 8'h33) begin
            $display("FAIL: replicated pattern gave %h %h %h %h", filled[0], filled[1], filled[2],
                     filled[3]);
            failures = failures + 1;
        end

        // An ascending packed range has its left index most significant.
        ascending = 8'b1000_0001;
        ascending[1] = 1'b1;
        if (ascending[0] !== 1'b1 || ascending !== 8'b1100_0001 || ascending[4:7] !== 4'b0001) begin
            $display("FAIL: ascending packed range, got %b", ascending);
            failures = failures + 1;
        end

        // Packed struct members are windows of the vector.
        pair = 8'h5a;
        pair.low = 4'h3;
        if (pair.high !== 4'h5 || pair !== 8'h53) begin
            $display("FAIL: packed struct members, got %h", pair);
            failures = failures + 1;
        end

        // An unpacked struct's members, including a two-state one, which an X does not reach.
        record = '{tag: 8'hab, count: 7, flags: 4'b1010};
        record.count = record.count + 1;
        record.flags = 4'b1x1x;
        if (record.tag !== 8'hab || record.count != 8 || record.flags !== 4'b1010) begin
            $display("FAIL: unpacked struct members, got %h %0d %b", record.tag, record.count,
                     record.flags);
            failures = failures + 1;
        end

        // A struct in an array, and a member of an element.
        for (int i = 0; i < 4; i++) records[i] = '{tag: 8'(i), count: i * 10, flags: 4'(i)};
        records[2].count = 99;
        address = 4'd2;
        if (records[address].count != 99 || records[3].tag !== 8'd3 || records[1].flags !== 4'd1)
        begin
            $display("FAIL: a struct in an array");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL aggregates: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS aggregates");
        $finish;
    end
endmodule
