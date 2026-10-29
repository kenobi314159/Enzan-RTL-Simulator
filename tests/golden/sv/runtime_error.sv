// SPDX-License-Identifier: GPL-3.0-or-later
//
// A design that reports an error while it runs and then finishes, which `enzan` must report
// with status 3, the design failing in simulation, rather than 2, which is for a design that
// did not analyse or elaborate.

module tb;
    initial begin
        $error("this design fails on purpose");
        $finish;
    end
endmodule
