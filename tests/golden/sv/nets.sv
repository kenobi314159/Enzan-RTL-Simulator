// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a net holds where nothing drives it: `z` for most kinds, and the pull or supply value
// for the rest. A partly driven bus shows both on one signal, because an undriven bit is its
// own default and not whatever its neighbour holds.

module tb;
    wire        w;
    tri         t;
    wand        wa;
    wor         wo;
    tri0        pulled_down;
    tri1        pulled_up;
    supply0     ground;
    supply1     power;
    wire  [3:0] bus;
    wand  [1:0] anded;

    logic [1:0] low;

    int failures = 0;

    assign bus[1:0] = low;
    assign anded    = low;

    initial begin
        low = 2'b10;
        #1;
        $display("w=%b t=%b wa=%b wo=%b tri0=%b tri1=%b supply0=%b supply1=%b", w, t, wa, wo,
                 pulled_down, pulled_up, ground, power);
        $display("bus=%b anded=%b", bus, anded);

        if (w !== 1'bz || t !== 1'bz || wa !== 1'bz || wo !== 1'bz) begin
            $display("FAIL: an undriven wire, tri, wand or wor should be z");
            failures = failures + 1;
        end
        if (pulled_down !== 1'b0 || pulled_up !== 1'b1) begin
            $display("FAIL: an undriven tri0 should be 0 and tri1 should be 1");
            failures = failures + 1;
        end
        if (ground !== 1'b0 || power !== 1'b1) begin
            $display("FAIL: supply0 should be 0 and supply1 should be 1");
            failures = failures + 1;
        end
        if (bus !== 4'bzz10) begin
            $display("FAIL: a bus driven in its low half should be zz10, got %b", bus);
            failures = failures + 1;
        end
        if (anded !== 2'b10) begin
            $display("FAIL: a wand with one driver should take its value, got %b", anded);
            failures = failures + 1;
        end

        low = 2'bz1;
        #1;
        $display("bus=%b anded=%b", bus, anded);
        if (bus !== 4'bzzz1 || anded !== 2'bz1) begin
            $display("FAIL: a single driver's z should reach the net, got %b and %b", bus, anded);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL nets: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS nets");
        $finish;
    end
endmodule
