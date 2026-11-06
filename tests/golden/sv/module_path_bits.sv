// SPDX-License-Identifier: GPL-3.0-or-later
//
// Module paths that differ between the bits of an output, as a carry chain's do in Yosys's models
// of AMD's cells: each bit takes the delay of its own paths (IEEE 1800 section 30.4), a bit with
// none changes at once, a parallel path between vectors pairs their bits, and a bit's pulse is
// rejected or let through as its own path's limits have it. Icarus Verilog applies a path to a
// bit to the whole vector, so the case checks itself.

`timescale 1ns / 100ps

module chain (input ci, input [1:0] s, output [2:0] o);
    assign o = {s[1] ^ s[0], s[1] ^ ci, s[0] ^ ci};
    specify
        (ci => o[0]) = 2;
        (s[0] => o[0]) = 1;
        (ci => o[1]) = 4;
        (s[1] => o[1]) = 3;
    endspecify
endmodule

module rejecting (input a, input b, output [1:0] y);
    assign y = {b, a};
    specify
        (a => y[0]) = 5;
        (b => y[1]) = 3;
    endspecify
endmodule

module passing (input a, input b, output [1:0] y);
    assign y = {b, a};
    specify
        specparam PATHPULSE$ = 0;
        (a => y[0]) = 5;
        (b => y[1]) = 3;
    endspecify
endmodule

module pairs (input [1:0] d, output [1:0] q);
    assign q = d;
    specify
        (d => q) = 2;
    endspecify
endmodule

module tb;
    logic ci = 1'b0;
    logic [1:0] s = 2'b00;
    wire [2:0] o;
    chain u_chain (.ci(ci), .s(s), .o(o));

    logic ra = 1'b0, rb = 1'b0, pa = 1'b0, pb = 1'b0;
    wire [1:0] ry, py;
    rejecting u_rejecting (.a(ra), .b(rb), .y(ry));
    passing u_passing (.a(pa), .b(pb), .y(py));

    logic [1:0] d = 2'b00;
    wire [1:0] q;
    pairs u_pairs (.d(d), .q(q));

    int failures = 0;
    task automatic expect3(int which, logic [2:0] got, logic [2:0] want);
        $display("%.1f: check %0d %b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    initial begin
        // The chain: o[0] is s[0] ^ ci, o[1] s[1] ^ ci and o[2], with no path, s[1] ^ s[0].
        #10 ci = 1'b1;                       // o[0] by ci's 2, o[1] by ci's 4
        #1 expect3(1, o, 3'b000);
        #2 expect3(2, o, 3'b001);
        #2 expect3(3, o, 3'b011);
        #5 s[1] = 1'b1;                      // o[1] by s[1]'s 3; o[2] at once
        #0.5 expect3(4, o, 3'b111);
        #3 expect3(5, o, 3'b101);
        #6.5;
        s[0] = 1'b1;                         // at 30, together: o[1] by ci's 4, its own path
        ci = 1'b0;                           // of the two, though s[0]'s to o[0] is 1
        #0.5 expect3(6, o, 3'b001);
        #3 expect3(7, o, 3'b001);
        #1 expect3(8, o, 3'b011);
        #5.5 s = 2'b00;                      // at 40: o[0] by s[0]'s 1, o[1] by s[1]'s 3
        #0.5 expect3(9, o, 3'b011);
        #1 expect3(10, o, 3'b010);
        #2 expect3(11, o, 3'b000);

        // A pulse of 1 against a delay of 5 on one bit, while the other bit changes by its own
        // path: rejected by the default limits, let through by PATHPULSE$ = 0.
        #6.5;
        {ra, rb, pa, pb} = 4'b1111;          // at 50
        #1 {ra, pa} = 2'b00;
        #2.5 expect3(12, {1'b0, ry}, 3'b010);
        expect3(13, {1'b0, py}, 3'b010);
        #2 expect3(14, {1'b0, ry}, 3'b010);
        expect3(15, {1'b0, py}, 3'b011);
        #1 expect3(16, {1'b0, py}, 3'b010);

        // A parallel path between vectors: each bit from its own.
        #3.5 d[0] = 1'b1;                    // at 60
        #1 d[1] = 1'b1;
        #1.5 expect3(17, {1'b0, q}, 3'b001);
        #1 expect3(18, {1'b0, q}, 3'b011);

        if (failures == 0) $display("PASS module_path_bits");
        $finish;
    end
endmodule
