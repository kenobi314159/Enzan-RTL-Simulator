// SPDX-License-Identifier: GPL-3.0-or-later
//
// `%m` prints the hierarchical name of the instance it is in, dot-separated (IEEE 1800 section
// 21.2.1.7): an instance, and instances in a generate loop.

// Each at a time of its own, so that the order is the same everywhere.
module leaf #(parameter integer AT = 1);
    initial #AT $display("in %m");
endmodule

module tb;
    leaf #(.AT(1)) u_leaf ();
    for (genvar i = 0; i < 2; i++) begin : lanes
        leaf #(.AT(2 + i)) u ();
    end

    initial begin
        $display("in %m");
        #4 $display("PASS scope_names");
        $finish;
    end
endmodule
