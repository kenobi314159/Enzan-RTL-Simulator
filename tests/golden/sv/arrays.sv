// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unpacked arrays, which Enzan stores flat with the left element highest: memories read and
// written at a variable index, both directions of declared range, two dimensions, and a wire array
// driven element by element as a lookup table often is. Assignment patterns, whole-array copy and
// equality, and an array through a port, which Icarus Verilog 12 does not implement or leaves X,
// and an index past an inner dimension, which it lets land in the next row, are in aggregates.sv.

module tb;
    logic [7:0] memory [0:15];
    logic [3:0] down [3:0];
    logic [7:0] grid [0:2][0:3];
    wire  [7:0] table_ [0:3];
    logic [3:0] address;
    logic [7:0] word;
    int failures = 0;

    assign table_[0] = 8'h63;
    assign table_[1] = 8'h7c;
    assign table_[2] = 8'h77;
    assign table_[3] = 8'h7b;


    initial begin
        for (int i = 0; i < 16; i++) memory[i] = 8'(i * 3);
        address = 4'd7;
        word = memory[address];
        $display("memory[7]=%0d memory[15]=%0d", word, memory[15]);
        if (word !== 8'd21 || memory[15] !== 8'd45) begin
            $display("FAIL: memory read back %0d and %0d", word, memory[15]);
            failures = failures + 1;
        end

        // Writing one element leaves its neighbours alone.
        memory[address] = 8'hee;
        if (memory[6] !== 8'd18 || memory[7] !== 8'hee || memory[8] !== 8'd24) begin
            $display("FAIL: a write to memory[7] disturbed its neighbours");
            failures = failures + 1;
        end

        // An index with an unknown bit reads X, and a write through one does nothing.
        address = 4'bx000;
        if (memory[address] !== 8'bxxxx_xxxx) begin
            $display("FAIL: an unknown index read %b", memory[address]);
            failures = failures + 1;
        end
        memory[address] = 8'h00;
        if (memory[0] !== 8'd0 || memory[8] !== 8'd24) begin
            $display("FAIL: a write through an unknown index landed somewhere");
            failures = failures + 1;
        end

        // A descending range indexes the same way from the other end.
        for (int i = 0; i < 4; i++) down[i] = 4'(i + 1);
        if (down[0] !== 4'd1 || down[3] !== 4'd4) begin
            $display("FAIL: descending array read %0d and %0d", down[0], down[3]);
            failures = failures + 1;
        end

        // Two dimensions.
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) grid[r][c] = 8'(r * 16 + c);
        $display("grid[1][2]=%h grid[2][3]=%h", grid[1][2], grid[2][3]);
        if (grid[1][2] !== 8'h12 || grid[2][3] !== 8'h23) begin
            $display("FAIL: two-dimensional reads");
            failures = failures + 1;
        end

        // A wire array driven element by element.
        #1;
        $display("table=%h %h", table_[0], table_[3]);
        if (table_[1] !== 8'h7c) begin
            $display("FAIL: wire array");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL arrays: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS arrays");
        $finish;
    end
endmodule
