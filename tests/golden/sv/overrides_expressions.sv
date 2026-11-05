// SPDX-License-Identifier: GPL-3.0-or-later
//
// The right-hand side of a procedural `assign` or `force` is evaluated again whenever anything it
// reads changes, for as long as the override stands (IEEE 1800 section 10.6). Icarus Verilog
// evaluates an expression there only once, so this case checks itself against the standard:
//
// - `follower` follows an expression of a variable while assigned, and not once deassigned.
// - `held` is forced over an assign, which is replaced while the force stands; the release
//   re-establishes the new one, evaluated with the values it reads then.
// - `pin` is a net forced to an expression of its own driver and of a signal in an instance.
// - `twice` is assigned by one statement and then another, and only the second still holds it.

module inner;
    logic [3:0] count = 4'h0;
endmodule

module tb;
    logic [7:0] source = 8'h10;
    logic [7:0] follower;

    logic [3:0] first = 4'h1, second = 4'h2, outer = 4'hf;
    logic [3:0] held;

    logic driven = 1'b0;
    wire [3:0] pin = {4{driven}};
    inner u_inner ();

    logic [3:0] a = 4'h3, b = 4'h5;
    logic [3:0] twice;

    int failures = 0;

    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures = failures + 1;
        end
    endtask

    initial begin
        assign follower = source + 8'h01;
        check(1, follower === 8'h11);
        source = 8'h20;
        #1 $display("follower=%h", follower);
        check(2, follower === 8'h21);
        deassign follower;
        source = 8'h30;
        #1 $display("deassigned follower=%h", follower);
        check(3, follower === 8'h21);

        assign held = first + second;
        #1 check(4, held === 4'h3);
        force held = outer ^ first;
        #1 $display("forced held=%h", held);
        check(5, held === 4'he);
        first = 4'h2;
        #1 check(6, held === 4'hd);
        assign held = second - first;
        second = 4'h7;
        #1 check(7, held === 4'hd);
        release held;
        #1 $display("released held=%h", held);
        check(8, held === 4'h5);
        first = 4'h3;
        #1 check(9, held === 4'h4);
        deassign held;

        force pin = ~{4{driven}} ^ u_inner.count;
        #1 $display("forced pin=%h", pin);
        check(10, pin === 4'hf);
        driven = 1'b1;
        #1 check(11, pin === 4'h0);
        u_inner.count = 4'h6;
        #1 $display("forced pin=%h", pin);
        check(12, pin === 4'h6);
        release pin;
        #1 check(13, pin === 4'hf);

        assign twice = a;
        assign twice = b;
        #1 check(14, twice === 4'h5);
        a = 4'h8;
        #1 check(15, twice === 4'h5);
        b = 4'h9;
        #1 $display("twice=%h", twice);
        check(16, twice === 4'h9);

        if (failures == 0) $display("PASS overrides_expressions");
        $finish;
    end
endmodule
