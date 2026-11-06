// SPDX-License-Identifier: GPL-3.0-or-later
//
// Procedural `assign` and `deassign`, `force` and `release` (IEEE 1800 section 10.6):
//
// - `q` is a flip-flop held at its initial value by an `assign` while a global reset is high, as
//   AMD's flip-flop models hold theirs, so its clock does nothing until the `deassign`.
// - `follower` is assigned another variable, which it follows until it is deassigned and keeps.
// - `held` is forced over an assign, and on release goes back to the assign's value.
// - `pin` is a net with one driver, forced to a value and to another signal, and released back to
//   its driver's value at once; `bus` has two, which resolve again on release.
//
// Every value here is a constant or a variable, because Icarus Verilog evaluates any other
// expression only once; overrides_expressions.sv checks that the rest are evaluated again.
// - `floating` is a net nothing drives and `pulled` a `tri0`, each back to its own on release.
// - `kept` is a variable, which a release leaves where the force put it.
// - `inner.state` is forced from above, by a reference down into an instance.
// - `data` is forced to x by a process that watches it, as AMD's memories mark a collision.

module inner (input logic clk, output logic [3:0] state);
    always_ff @(posedge clk) state <= state + 4'd1;
    initial state = 4'd0;
endmodule

module tb;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    // As AMD's `glbl` has it, a net the reset variable drives, so the reset rises a delta after
    // time zero, when every process is waiting for it.
    logic gsr_int = 1'b0;
    wire gsr = gsr_int;
    initial gsr_int = 1'b1;
    logic d = 1'b0;
    reg q;
    always @(gsr)
        if (gsr) assign q = 1'b1;
        else deassign q;
    always @(posedge clk) q <= d;

    logic [7:0] source = 8'h11;
    logic [7:0] follower;

    logic [3:0] inner_value = 4'h3, outer_value = 4'h9;
    logic [3:0] held;

    logic driven = 1'b1, other = 1'b0;
    wire pin = driven;

    logic left = 1'bz, right = 1'bz;
    wire bus;
    assign bus = left;
    assign bus = right;

    wire floating;
    tri0 pulled;

    logic [3:0] kept = 4'h0;

    logic enable = 1'b0;
    wire [3:0] data_in = 4'h5;
    wire [3:0] data = data_in;
    always @(enable or data)
        if (enable) force data = {4{1'bx}};
        else release data;

    wire [3:0] state;
    inner u_inner (.clk(clk), .state(state));

    int failures = 0;

    task automatic check(int which, bit holds);
        if (!holds) begin
            $display("FAIL: check %0d", which);
            failures = failures + 1;
        end
    endtask

    initial begin
        // The reset holds `q` through two clock edges.
        #1 d = 1'b0;
        #20 $display("t=%0d q=%b", $time, q);
        check(1, q === 1'b1);
        gsr_int = 1'b0;
        #1 $display("t=%0d deassigned q=%b", $time, q);
        check(2, q === 1'b1);
        #10 $display("t=%0d clocked q=%b", $time, q);
        check(3, q === 1'b0);

        // An assign follows what it assigns, and a deassign keeps the value it last had.
        follower = 8'h00;
        assign follower = source;
        $display("assigned follower=%h", follower);
        check(4, follower === 8'h11);
        source = 8'h40;
        #1 $display("followed follower=%h", follower);
        check(5, follower === 8'h40);
        follower = 8'hff;
        #1 check(6, follower === 8'h40);
        deassign follower;
        source = 8'h50;
        #1 $display("deassigned follower=%h", follower);
        check(7, follower === 8'h40);
        follower = 8'h07;
        #1 check(8, follower === 8'h07);

        // A force stands over an assign, and the assign takes the variable back on release.
        assign held = inner_value;
        #1 force held = outer_value;
        #1 $display("forced held=%h", held);
        check(9, held === 4'h9);
        inner_value = 4'h4;
        #1 check(10, held === 4'h9);
        release held;
        #1 $display("released held=%h", held);
        check(11, held === 4'h4);
        inner_value = 4'h6;
        #1 check(12, held === 4'h6);
        deassign held;

        // A forced net ignores its driver, and is released straight back to it.
        force pin = 1'b0;
        driven = 1'b1;
        #1 $display("forced pin=%b", pin);
        check(13, pin === 1'b0);
        force pin = other;
        other = 1'b1;
        driven = 1'b0;
        #1 check(14, pin === 1'b1);
        release pin;
        $display("released pin=%b", pin);
        check(15, pin === 1'b0);
        driven = 1'b1;
        #1 check(16, pin === 1'b1);

        left = 1'b1;
        force bus = 1'b0;
        #1 check(17, bus === 1'b0);
        right = 1'b0;
        release bus;
        #1 $display("released bus=%b", bus);
        check(18, bus === 1'bx);

        force floating = 1'b1;
        force pulled = 1'b1;
        #1 check(19, floating === 1'b1);
        check(20, pulled === 1'b1);
        release floating;
        release pulled;
        #1 $display("released floating=%b pulled=%b", floating, pulled);
        check(21, floating === 1'bz);
        check(22, pulled === 1'b0);

        // A released variable keeps its forced value until it is next written.
        force kept = 4'ha;
        kept = 4'h1;
        #1 check(23, kept === 4'ha);
        release kept;
        #1 $display("released kept=%h", kept);
        check(24, kept === 4'ha);
        kept = 4'h2;
        #1 check(25, kept === 4'h2);

        // Through a reference down into an instance, whose counter stops while forced.
        @(negedge clk) force u_inner.state = 4'hc;
        @(negedge clk) $display("forced state=%h", state);
        check(26, state === 4'hc);
        release u_inner.state;
        @(negedge clk) $display("released state=%h", state);
        check(27, state === 4'hd);

        // A process that forces what it watches, and releases it.
        enable = 1'b1;
        #1 $display("collision data=%b", data);
        check(28, data === 4'bxxxx);
        enable = 1'b0;
        #1 $display("no collision data=%b", data);
        check(29, data === 4'h5);

        if (failures == 0) $display("PASS overrides");
        $finish;
    end
endmodule
