// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Verilog-2001 function, static by default, whose scratch variable is one variable every call
// shares (IEEE 1800 section 13.3.1), called from two continuous assignments and two always_comb
// blocks. The calls write the scratch variable, so none of them is its continuous driver, and
// none of the callers waits on it: each call writes it before reading it, and a caller woken by
// another's write would wake that one in turn, for ever.

module tb;
    function [7:0] swap(input [7:0] value);
        reg [7:0] scratch;
        begin
            scratch = {value[3:0], value[7:4]};
            swap = scratch;
        end
    endfunction

    logic [7:0] a = 8'h12;
    logic [7:0] b = 8'h34;
    logic [7:0] swapped_a;
    logic [7:0] swapped_b;
    logic [7:0] swapped_xor;
    logic [7:0] swapped_or;
    assign swapped_a = swap(a);
    assign swapped_b = swap(b);
    always_comb swapped_xor = swap(a ^ b);
    always_comb swapped_or = swap(a | b);

    int failures = 0;

    initial begin
        #1;
        if (swapped_a !== 8'h21 || swapped_b !== 8'h43 || swapped_xor !== 8'h62
            || swapped_or !== 8'h63) begin
            $display("FAIL: callers sharing a static function computed %h %h %h %h", swapped_a,
                     swapped_b, swapped_xor, swapped_or);
            failures = failures + 1;
        end
        a = 8'hf0;
        #1;
        if (swapped_a !== 8'h0f || swapped_b !== 8'h43 || swapped_xor !== 8'h4c
            || swapped_or !== 8'h4f) begin
            $display("FAIL: callers sharing a static function recomputed %h %h %h %h",
                     swapped_a, swapped_b, swapped_xor, swapped_or);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL static_function_callers: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS static_function_callers");
        $finish;
    end
endmodule
