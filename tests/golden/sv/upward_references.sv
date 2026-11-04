// SPDX-License-Identifier: GPL-3.0-or-later
//
// References leaving the instance that holds them, from instances of one parameterisation placed
// where those references lead to different objects. Enzan translates a parameterisation once and
// shares it, so a reference that held for the first instance translated and not for another
// would show here as the other reading the wrong object, or nothing.
//
// - `probe` reads `tb.clk`, and waits on its edges, from one level down and from two. The level
//   between has a `clk` of its own, which a path counting the shallow instance's depth would
//   reach from the deep one.
// - `caller` calls `bump`, found by its name upwards: from below `tb` that is `tb`'s task, and
//   from below `shelter`, at the same distance, `shelter`'s.
// - `watcher` reads `holder.state`, its ancestor's, found by the ancestor's module name: from one
//   level below one holder, from two below the same holder, and from below a second holder.
// - `looker` reads `tb.u_seen.mark`, which from `u_seen` itself is its own `mark` and from
//   `u_other` is its sibling's.

module probe (output logic seen, output logic [3:0] edges);
    assign seen = tb.clk;
    initial edges = 4'd0;
    always @(posedge tb.clk) edges <= edges + 4'd1;
endmodule

module caller (input logic go);
    always @(posedge go) bump();
endmodule

module shelter (output logic seen, output logic [3:0] edges);
    logic clk = 1'b1;
    int hits = 0;
    task bump;
        hits = hits + 100;
    endtask
    probe u (.seen(seen), .edges(edges));
    caller c (.go(tb.clk));
endmodule

module watcher (output logic [3:0] seen);
    assign seen = holder.state;
endmodule

module nest (output logic [3:0] seen);
    watcher w (.seen(seen));
endmodule

module holder #(parameter logic [3:0] START = 4'd0) (
    input  logic       clk,
    output logic [3:0] near,
    output logic [3:0] far
);
    logic [3:0] state = START;
    always @(posedge clk) state <= state + 4'd1;

    watcher direct (.seen(near));
    nest    inner  (.seen(far));
endmodule

module looker #(parameter logic [3:0] OWN = 4'd0) (output logic [3:0] seen);
    logic [3:0] mark = OWN;
    assign seen = tb.u_seen.mark;
endmodule

module tb;
    logic clk = 1'b0;
    logic shallow, deep;
    logic [3:0] shallow_edges, deep_edges;
    int hits = 0;
    logic [3:0] near_a, far_a, near_b, far_b;
    logic [3:0] seen_by_self, seen_by_other;
    int failures = 0;

    task bump;
        hits = hits + 1;
    endtask

    // The deep instances first, so that they are the ones translated first.
    shelter u_shelter (.seen(deep), .edges(deep_edges));
    probe   u_probe   (.seen(shallow), .edges(shallow_edges));
    caller  u_caller  (.go(clk));

    holder #(.START(4'd2)) h_a (.clk(clk), .near(near_a), .far(far_a));
    holder #(.START(4'd2)) h_b (.clk(clk), .near(near_b), .far(far_b));

    looker #(.OWN(4'd7)) u_seen  (.seen(seen_by_self));
    looker #(.OWN(4'd7)) u_other (.seen(seen_by_other));

    task automatic check(int which, logic [3:0] actual, logic [3:0] expected);
        if (actual !== expected) begin
            $display("FAIL: check %0d gave %b, should be %b", which, actual, expected);
            failures = failures + 1;
        end
    endtask

    initial begin
        #1 $display("shallow=%b deep=%b", shallow, deep);
        check(1, {3'b0, shallow}, 4'd0);
        check(2, {3'b0, deep}, 4'd0);
        clk = 1'b1;
        #1 $display("shallow=%b deep=%b", shallow, deep);
        check(3, {3'b0, shallow}, 4'd1);
        check(4, {3'b0, deep}, 4'd1);
        $display("shallow_edges=%0d deep_edges=%0d hits=%0d shelter_hits=%0d", shallow_edges,
                 deep_edges, hits, u_shelter.hits);
        check(11, shallow_edges, 4'd1);
        check(12, deep_edges, 4'd1);
        check(13, hits[3:0], 4'd1);
        if (u_shelter.hits != 100) begin
            $display("FAIL: shelter's task was called %0d times", u_shelter.hits / 100);
            failures = failures + 1;
        end

        // One clock edge has passed: both holders count from 2 to 3. Then b is set apart.
        h_b.state = 4'd9;
        #1 $display("near_a=%0d far_a=%0d near_b=%0d far_b=%0d", near_a, far_a, near_b, far_b);
        check(5, near_a, 4'd3);
        check(6, far_a, 4'd3);
        check(7, near_b, 4'd9);
        check(8, far_b, 4'd9);

        u_seen.mark = 4'd5;
        #1 $display("seen_by_self=%0d seen_by_other=%0d", seen_by_self, seen_by_other);
        check(9, seen_by_self, 4'd5);
        check(10, seen_by_other, 4'd5);

        if (failures == 0) $display("PASS upward_references");
        $finish;
    end
endmodule
