// SPDX-License-Identifier: GPL-3.0-or-later
//
// `defparam` (IEEE 1800 section 23.10.1) as Quartus's netlists set every cell's parameters: a
// string longer than the parameter's default, which takes the type of its final value (section
// 6.20.2), as an override on the instance does; a number; several parameters of one instance;
// a defparam reaching two levels down, of the default's width; and defparams inside generate
// blocks, each setting an instance beside it, as Altera's altsyncram sets its body's.

module ram_cell;
    parameter mode = "single_port";
    parameter width = 4;
    parameter [7:0] mask = 8'hff;
    reg dual;

    initial dual = mode == "bidir_dual_port";
endmodule

module pair;
    ram_cell inner ();
endmodule

module generated #(parameter dual = 1);
    if (dual) begin : m_dual
        ram_cell ram ();
        defparam ram.mode = "bidir_dual_port";
    end else begin : m_single
        ram_cell ram ();
        defparam ram.mode = "single";
    end
    for (genvar i = 0; i < 2; i = i + 1) begin : row
        ram_cell ram ();
        defparam ram.width = i + 5, ram.mode = "bidir_dual_port";
    end
endmodule

module tb;
    ram_cell by_defparam ();
    defparam by_defparam.mode = "bidir_dual_port";
    defparam by_defparam.width = 9, by_defparam.mask = 8'h0f;

    ram_cell #(.mode("bidir_dual_port")) by_override ();
    ram_cell untouched ();

    pair outer ();
    defparam outer.inner.mode = "rom_or_dual";

    generated gen ();

    initial begin
        #2;
        $display("by_defparam: mode=%0s dual=%b bits=%0d width=%0d mask=%h", by_defparam.mode,
                 by_defparam.dual, $bits(by_defparam.mode), by_defparam.width, by_defparam.mask);
        $display("by_override: mode=%0s dual=%b bits=%0d", by_override.mode, by_override.dual,
                 $bits(by_override.mode));
        $display("untouched: mode=%0s dual=%b bits=%0d", untouched.mode, untouched.dual,
                 $bits(untouched.mode));
        $display("outer.inner: mode=%0s bits=%0d", outer.inner.mode, $bits(outer.inner.mode));
        $display("gen.m_dual.ram: mode=%0s dual=%b bits=%0d", gen.m_dual.ram.mode,
                 gen.m_dual.ram.dual, $bits(gen.m_dual.ram.mode));
        $display("gen.row: width=%0d,%0d dual=%b,%b", gen.row[0].ram.width,
                 gen.row[1].ram.width, gen.row[0].ram.dual, gen.row[1].ram.dual);
        if (by_defparam.dual === 1'b1 && by_override.dual === 1'b1 && untouched.dual === 1'b0
            && by_defparam.width == 9 && by_defparam.mask == 8'h0f
            && outer.inner.mode == "rom_or_dual" && gen.m_dual.ram.dual === 1'b1
            && gen.row[0].ram.width == 5 && gen.row[1].ram.width == 6
            && gen.row[0].ram.dual === 1'b1 && gen.row[1].ram.dual === 1'b1)
            $display("PASS defparams");
        $finish;
    end
endmodule
