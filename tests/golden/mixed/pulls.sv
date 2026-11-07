// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pull-up as AMD's PULLUP is one, a pullup through a buffer of weak strengths; a pull-down, a
// pulldown alone; and a driver that drives strongly or releases, for pulls.vhd to put on signals
// of its own.

module pull_up (output o);
    wire a;
    pullup (a);
    buf (weak0, weak1) (o, a);
endmodule

module pull_down (output o);
    pulldown (o);
endmodule

module driver (input i, input t, output o);
    assign o = t ? 1'bz : i;
endmodule
