// SPDX-License-Identifier: GPL-3.0-or-later
//
// A synchronous FIFO with a self-checking testbench. Together with counter.sv this is the
// pair DESIGN_GUIDE.md section 18 names as M1's exit criterion.
//
// The storage is a *packed* array, `logic [DEPTH-1:0][WIDTH-1:0]`, because a packed type is
// a bit vector and needs no aggregate support in the value engine. An unpacked `logic
// [WIDTH-1:0] mem [DEPTH]` is the more idiomatic spelling and arrives with M5.

module fifo #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 4
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             push,
    input  logic [WIDTH-1:0] data_in,
    input  logic             pop,
    output logic [WIDTH-1:0] data_out,
    output logic             empty,
    output logic             full
);
    localparam int PtrBits = 2;

    logic [DEPTH-1:0][WIDTH-1:0] mem;
    logic [PtrBits-1:0]          read_ptr;
    logic [PtrBits-1:0]          write_ptr;
    logic [PtrBits:0]            level;

    assign empty = (level == '0);
    assign full = (level == DEPTH[PtrBits:0]);
    assign data_out = mem[read_ptr];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            read_ptr  <= '0;
            write_ptr <= '0;
            level     <= '0;
        end
        else begin
            if (push && !full) begin
                mem[write_ptr] <= data_in;
                write_ptr <= write_ptr + 1'b1;
            end

            if (pop && !empty)
                read_ptr <= read_ptr + 1'b1;

            // The level moves by at most one per cycle, and a simultaneous push and pop
            // leaves it unchanged.
            if (push && !full && !(pop && !empty))
                level <= level + 1'b1;
            else if (pop && !empty && !(push && !full))
                level <= level - 1'b1;
        end
    end
endmodule

module tb;
    localparam int Width = 8;
    localparam int Depth = 4;

    logic             clk = 1'b0;
    logic             rst_n = 1'b0;
    logic             push = 1'b0;
    logic             pop = 1'b0;
    logic [Width-1:0] data_in = '0;
    logic [Width-1:0] data_out;
    logic             empty;
    logic             full;
    int               failures = 0;

    fifo #(.WIDTH(Width), .DEPTH(Depth)) u_dut (
        .clk(clk),
        .rst_n(rst_n),
        .push(push),
        .data_in(data_in),
        .pop(pop),
        .data_out(data_out),
        .empty(empty),
        .full(full)
    );

    always #5 clk = ~clk;

    initial begin
        #12 rst_n = 1'b1;

        if (empty !== 1'b1) begin
            $display("FAIL: not empty after reset");
            failures = failures + 1;
        end
        if (full !== 1'b0) begin
            $display("FAIL: full after reset");
            failures = failures + 1;
        end

        // Fill it, one word per clock.
        push = 1'b1;
        data_in = 8'hA0;
        #10 data_in = 8'hA1;
        #10 data_in = 8'hA2;
        #10 data_in = 8'hA3;
        #10 push = 1'b0;

        if (full !== 1'b1) begin
            $display("FAIL: not full after %0d pushes, level says otherwise", Depth);
            failures = failures + 1;
        end
        if (empty !== 1'b0) begin
            $display("FAIL: empty while full");
            failures = failures + 1;
        end

        // A push into a full FIFO must be ignored rather than corrupting the head.
        push = 1'b1;
        data_in = 8'hFF;
        #10 push = 1'b0;

        if (data_out !== 8'hA0) begin
            $display("FAIL: head is %0h, expected a0", data_out);
            failures = failures + 1;
        end

        // Drain it, checking each word in order.
        pop = 1'b1;
        #10;
        if (data_out !== 8'hA1) begin
            $display("FAIL: second word is %0h, expected a1", data_out);
            failures = failures + 1;
        end
        #10;
        if (data_out !== 8'hA2) begin
            $display("FAIL: third word is %0h, expected a2", data_out);
            failures = failures + 1;
        end
        #10;
        if (data_out !== 8'hA3) begin
            $display("FAIL: fourth word is %0h, expected a3", data_out);
            failures = failures + 1;
        end
        #10 pop = 1'b0;

        if (empty !== 1'b1) begin
            $display("FAIL: not empty after draining");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL fifo: %0d checks failed", failures);
            $fatal(1);
        end

        $display("PASS fifo");
        $finish;
    end
endmodule
