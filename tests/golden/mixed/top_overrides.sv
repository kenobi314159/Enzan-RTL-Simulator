// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SystemVerilog half of top_overrides.vhd.

module leaf #(parameter int P = 0) ();
    initial if (P != 10) $error("P is %0d", P);
endmodule

module sv_tb #(parameter int WIDTH = 1) ();
    vleaf #(.K(WIDTH + 1)) u ();

    initial begin
        if (WIDTH != 3) $error("WIDTH is %0d", WIDTH);
        #1 $display("PASS sv_tb");
        $finish;
    end
endmodule
