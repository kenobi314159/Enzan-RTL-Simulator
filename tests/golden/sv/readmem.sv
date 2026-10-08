// SPDX-License-Identifier: GPL-3.0-or-later
//
// `$readmemh` and `$readmemb` (IEEE 1800 section 21.4), run by run-isolated.sh, which compares
// the transcript with Icarus Verilog's. The design writes its own data files first. Without a
// range, words load from the lowest address up, whichever way the memory is declared. A range
// loads from its start towards its end, either way round. `@` moves to an address, and an
// element no word reaches keeps its value.

module tb;
    logic [7:0]  ascending [0:7];
    logic [7:0]  descending [7:0];
    logic [7:0]  ranged [0:7];
    logic [7:0]  reversed [0:7];
    logic [11:0] wide [1:4];
    logic [3:0]  nibbles [2:5];
    integer file;

    initial begin
        file = $fopen("words.hex", "w");
        $fdisplay(file, "// four words, an address, and two more");
        $fdisplay(file, "01 2 /* inline */ 0_3");
        $fdisplay(file, "f4");
        $fdisplay(file, "@6");
        $fdisplay(file, "x6 zz");
        $fclose(file);

        file = $fopen("short.hex", "w");
        $fdisplay(file, "a1 b2 c3");
        $fclose(file);

        file = $fopen("bits.bin", "w");
        $fdisplay(file, "0001 1x1_0");
        $fdisplay(file, "z111");
        $fclose(file);

        foreach (ranged[i]) ranged[i] = 8'hee;

        $readmemh("words.hex", ascending);
        $readmemh("words.hex", descending);
        $readmemh("short.hex", ranged, 2, 4);
        $readmemh("short.hex", reversed, 6, 4);
        $readmemh("short.hex", wide);
        $readmemb("bits.bin", nibbles);

        for (int i = 0; i < 8; i++)
            $display("[%0d] ascending=%h descending=%h ranged=%h reversed=%h", i, ascending[i],
                     descending[i], ranged[i], reversed[i]);
        for (int i = 1; i <= 4; i++) $display("wide[%0d]=%h", i, wide[i]);
        for (int i = 2; i <= 5; i++) $display("nibbles[%0d]=%b", i, nibbles[i]);
        $finish;
    end
endmodule
