// SPDX-License-Identifier: GPL-3.0-or-later
//
// MOS switches (IEEE 1800 section 28.7): `nmos` and `pmos` over every value of their input and
// control, `cmos` over its two controls, and the strength a switch passes from its input (section
// 28.13), strong against a pull-up and weak against a strong driver. Altera's I/O buffers drive
// their outputs through a `pmos` held on, which passes `z`. An unknown control gives the
// standard's `L` or `H`, which both simulators print as `x`.

module tb;
    reg d, c, n, p;
    wire o_n, o_p, o_c;
    nmos (o_n, d, c);
    pmos (o_p, d, c);
    cmos (o_c, d, n, p);

    // Strong through the switch beats the pull-up; off, the pull-up holds the net.
    reg s;
    wire pulled;
    pmos (pulled, s, c);
    pullup (pulled);

    // Weak in, weak out: a strong driver on the far side wins while it drives.
    reg v, enable, strong_value;
    wire weak_in, shared;
    assign (weak0, weak1) weak_in = v;
    nmos (shared, weak_in, 1'b1);
    assign shared = enable ? strong_value : 1'bz;

    // A switch held on, as Altera's buffers are, and one with a delay.
    reg i;
    wire held_on, delayed;
    pmos (held_on, i, 1'b0);
    nmos #3 (delayed, d, c);

    reg [3:0] values = 4'b0000;
    integer x, y, failures = 0;
    initial begin
        for (x = 0; x < 4; x = x + 1) begin
            for (y = 0; y < 4; y = y + 1) begin
                d = x == 0 ? 1'b0 : x == 1 ? 1'b1 : x == 2 ? 1'bx : 1'bz;
                c = y == 0 ? 1'b0 : y == 1 ? 1'b1 : y == 2 ? 1'bx : 1'bz;
                #5 $display("d=%b c=%b: nmos %b pmos %b", d, c, o_n, o_p);
            end
        end
        d = 1'b1;
        for (x = 0; x < 3; x = x + 1) begin
            for (y = 0; y < 3; y = y + 1) begin
                n = x == 0 ? 1'b0 : x == 1 ? 1'b1 : 1'bx;
                p = y == 0 ? 1'b0 : y == 1 ? 1'b1 : 1'bx;
                #5 $display("n=%b p=%b: cmos %b", n, p, o_c);
            end
        end
        d = 1'bz;
        n = 1'b1;
        #5 $display("d=z n=1: cmos %b", o_c);
        if (o_c !== 1'bz) failures = failures + 1;

        s = 1'b0;
        c = 1'b0;
        #5 $display("on, 0 in: pulled %b", pulled);
        if (pulled !== 1'b0) failures = failures + 1;
        c = 1'b1;
        #5 $display("off: pulled %b", pulled);
        if (pulled !== 1'b1) failures = failures + 1;

        v = 1'b0;
        enable = 1'b1;
        strong_value = 1'b1;
        #5 $display("weak 0 against strong 1: %b", shared);
        if (shared !== 1'b1) failures = failures + 1;
        enable = 1'b0;
        #5 $display("weak 0 alone: %b", shared);
        if (shared !== 1'b0) failures = failures + 1;

        i = 1'bz;
        #5 $display("held on, z in: %b", held_on);
        if (held_on !== 1'bz) failures = failures + 1;
        i = 1'b1;
        #5 $display("held on, 1 in: %b", held_on);
        if (held_on !== 1'b1) failures = failures + 1;

        d = 1'b0;
        c = 1'b1;
        #2 $display("delayed, after 2: %b", delayed);
        #2 $display("delayed, after 4: %b", delayed);
        if (delayed !== 1'b0) failures = failures + 1;

        if (failures == 0) $display("PASS switches");
        $finish;
    end
endmodule
