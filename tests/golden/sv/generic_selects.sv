// SPDX-License-Identifier: GPL-3.0-or-later
//
// Selects in objects whose width and depth are parameters, in a generic unit (DESIGN_GUIDE.md
// section 18, M5.1): a memory written and read at a moving index, whose element and stride are
// the parameter's width, and a bit and a part at fixed positions of a parameter-wide vector.
// Each instance must address its own memory by its own stride.

module fifo #(parameter int W = 8, parameter int DEPTH = 4, parameter int AW = 2) (
    input  logic          clk,
    input  logic          rst,
    input  logic          push,
    input  logic [W-1:0]  din,
    input  logic [AW-1:0] rd,
    output logic [W-1:0]  dout,
    output logic          low_bit,
    output logic [3:0]    low_nibble
);
    logic [W-1:0] mem [DEPTH];
    logic [AW-1:0] wr;
    assign dout = mem[rd];
    assign low_bit = dout[0];
    assign low_nibble = dout[3:0];
    always_ff @(posedge clk) begin
        if (rst) wr <= '0;
        else if (push) begin
            mem[wr] <= din;
            wr <= wr + 1'b1;
        end
    end
endmodule

module tb;
    logic clk = 1'b0;
    logic rst = 1'b1;
    logic push = 1'b0;
    logic [7:0] d8, q8;
    logic [15:0] d16, q16;
    logic [1:0] rd2;
    logic [2:0] rd3;
    logic b8, b16;
    logic [3:0] n8, n16;
    int failures = 0;

    fifo u8 (.clk, .rst, .push, .din(d8), .rd(rd2), .dout(q8), .low_bit(b8), .low_nibble(n8));
    fifo #(.W(16), .DEPTH(8), .AW(3)) u16 (.clk, .rst, .push, .din(d16), .rd(rd3), .dout(q16),
                                           .low_bit(b16), .low_nibble(n16));

    always #5 clk = ~clk;

    initial begin
        #12 rst = 1'b0;
        push = 1'b1;
        for (int i = 0; i < 6; i++) begin
            d8 = 8'(i * 37 + 1);
            d16 = 16'(i * 4099 + 3);
            @(posedge clk);
            #1;
        end
        push = 1'b0;
        // Six pushes: the four-deep memory has wrapped and holds pushes 4, 5, 2 and 3; the
        // eight-deep one holds all six.
        for (int i = 0; i < 6; i++) begin
            rd2 = 2'(i);
            rd3 = 3'(i);
            #1;
            $display("i=%0d q8=%h b8=%b n8=%h q16=%h b16=%b n16=%h", i, q8, b8, n8, q16, b16, n16);
            if (i < 4 && q8 !== 8'(((i < 2) ? i + 4 : i) * 37 + 1)) begin
                $display("FAIL: u8 at %0d", i);
                failures = failures + 1;
            end
            if (q16 !== 16'(i * 4099 + 3) || n16 !== q16[3:0] || b16 !== q16[0]) begin
                $display("FAIL: u16 at %0d", i);
                failures = failures + 1;
            end
        end
        if (failures != 0) $fatal(1, "FAIL generic_selects: %0d checks failed", failures);
        $display("PASS generic_selects");
        $finish;
    end
endmodule
