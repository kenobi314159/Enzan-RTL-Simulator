// SPDX-License-Identifier: GPL-3.0-or-later
//
// Widths a package's constant functions compute from parameters, in a generic unit
// (DESIGN_GUIDE.md section 18, M5.1): the unit is translated once and each function becomes a
// unit of its own, run at elaboration for every parameterisation an instance asks for. An
// automatic function calling another sizes a local parameter, and a static one with a loop sizes
// a declaration directly.

package widths;
    function automatic int unsigned index_width(input int unsigned count);
        if (count > 32'd1) return unsigned'($clog2(count));
        return 32'd1;
    endfunction

    function automatic int unsigned count_width(input int unsigned largest);
        return index_width(largest + 1);
    endfunction

    // Verilog's form: static, its result assigned by name, its local reached by a loop.
    function integer bits_for(input integer value);
        integer remaining;
        begin
            bits_for = 0;
            for (remaining = value; remaining > 0; remaining = remaining / 2)
                bits_for = bits_for + 1;
        end
    endfunction
endpackage

module wrap_counter #(parameter int unsigned N = 4,
                      localparam int unsigned W = widths::count_width(N)) (
    input  logic         clk,
    input  logic         rst,
    output logic [W-1:0] count,
    output logic         wrapped
);
    // As wide as N itself needs, which is W too for every N but a power of two minus one.
    logic [widths::bits_for(N)-1:0] limit;
    assign limit = N;
    assign wrapped = count == limit;
    always_ff @(posedge clk) begin
        if (rst || wrapped) count <= '0;
        else count <= count + 1'b1;
    end
endmodule

module tb;
    logic clk = 1'b0;
    logic rst = 1'b1;
    logic [2:0] five;
    logic [4:0] twenty;
    logic [0:0] one;
    logic [6:0] hundred;
    logic five_wrapped, twenty_wrapped, one_wrapped, hundred_wrapped;
    int failures = 0;

    wrap_counter #(.N(5)) u_five (.clk, .rst, .count(five), .wrapped(five_wrapped));
    wrap_counter #(.N(20)) u_twenty (.clk, .rst, .count(twenty), .wrapped(twenty_wrapped));
    wrap_counter #(.N(1)) u_one (.clk, .rst, .count(one), .wrapped(one_wrapped));
    wrap_counter #(.N(100)) u_hundred (.clk, .rst, .count(hundred), .wrapped(hundred_wrapped));

    always #5 clk = ~clk;

    task automatic check(int which, int got, int expected);
        if (got != expected) begin
            $display("FAIL: check %0d is %0d, expected %0d", which, got, expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #12 rst = 1'b0;
        #1000;
        $display("five=%0d twenty=%0d one=%0d hundred=%0d", five, twenty, one, hundred);
        $display("widths %0d %0d %0d %0d, limits %0d %0d %0d %0d", $bits(u_five.count),
                 $bits(u_twenty.count), $bits(u_one.count), $bits(u_hundred.count),
                 $bits(u_five.limit), $bits(u_twenty.limit), $bits(u_one.limit),
                 $bits(u_hundred.limit));
        // A hundred rising edges after reset, each counter wrapping after N + 1 of them.
        check(1, int'(five), 100 % 6);
        check(2, int'(twenty), 100 % 21);
        check(3, int'(one), 100 % 2);
        check(4, int'(hundred), 100 % 101);
        check(5, $bits(u_five.count), 3);
        check(6, $bits(u_twenty.count), 5);
        check(7, $bits(u_one.count), 1);
        check(8, $bits(u_hundred.count), 7);
        check(9, $bits(u_twenty.limit), 5);
        check(10, $bits(u_one.limit), 1);
        if (failures != 0) $fatal(1, "FAIL generic_functions: %0d checks failed", failures);
        $display("PASS generic_functions");
        $finish;
    end
endmodule
