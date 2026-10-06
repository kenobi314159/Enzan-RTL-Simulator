// SPDX-License-Identifier: GPL-3.0-or-later
//
// Constructs a parameter makes degenerate, in code a constant condition rules out: the branch of
// a procedural `if` its condition does not take, and the arm of a `?:` its condition does not
// select. IEEE 1800 makes each an error even there, and Enzan accepts it with a warning
// (ENZ1008), as Icarus Verilog does for most of them; the code never runs, so it is never
// translated. A reversed range select, which slang already accepts in such code, runs too.

module pad #(parameter int W = 0, parameter int N = 4) (
    input  logic [7:0] x,
    output logic [7:0] y
);
    always_comb begin
        if (W > 0) y = x[7 -: W];
        else y = x;
    end
endmodule

module tb;
    localparam int W = 0;
    localparam int N = 4;

    logic [7:0] x = 8'h5a;
    logic [7:0] indexed, padded, repeated, cast, reversed, selected;
    logic [7:0] pattern [0:1];
    logic [7:0] narrow, wide;
    int failures = 0;

    // Dead in one instance and live, and valid, in the other.
    pad #(.W(0)) u_dead (.x, .y(narrow));
    pad #(.W(4)) u_live (.x, .y(wide));

    initial begin
        if (W > 0) indexed = x[7 -: W];
        else indexed = x;

        if (W > N) padded = {{(W - N){1'b0}}, x};
        else padded = x;

        if (W > 0) repeated = {W{1'b1}};
        else repeated = x;

        if (W > 0) cast = W'(x);
        else cast = x;

        if (W > 0) pattern = '{W{8'h00}};
        else pattern = '{x, x};

        if (W > 0) reversed = x[0:3];
        else reversed = x;

        selected = W > 0 ? x[7 -: W] : x;

        #1;
        $display("%h %h %h %h %h %h %h %h %h", indexed, padded, repeated, cast, reversed, selected,
                 pattern[1], narrow, wide);
        if (indexed !== x || padded !== x || repeated !== x || cast !== x || reversed !== x
            || selected !== x || pattern[1] !== x) begin
            $display("FAIL: a construct a constant condition rules out changed the result");
            failures = failures + 1;
        end
        if (narrow !== x || wide !== 8'h05) begin
            $display("FAIL: the instances gave %h and %h", narrow, wide);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL dead_code: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS dead_code");
        $finish;
    end
endmodule
