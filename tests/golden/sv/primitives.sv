// SPDX-License-Identifier: GPL-3.0-or-later
//
// User-defined primitives (IEEE 1800 section 29), each kind AMD's and Altera's models use:
//
// - `mux2` is combinational, as AMD's LUT1 to LUT3 are, with rows for an unknown select whose
//   inputs agree; an input combination no row has is x, and a z input reads as x.
// - `latch` is level-sensitive, as AMD's LDCE is: transparent while its gate is 1, holding while
//   it is 0, with a row that keeps the state where the data agrees with it.
// - `dff` is edge-sensitive, as Altera's atoms are, with an asynchronous clear that takes
//   precedence, an initial state, and rows that ignore the data's changes and the clock's fall.

primitive mux2 (o, d1, d0, s);
    output o;
    input d1, d0, s;
    table
    // d1 d0 s : o
        ?  1  0 : 1;
        ?  0  0 : 0;
        1  ?  1 : 1;
        0  ?  1 : 0;
        0  0  x : 0;
        1  1  x : 1;
    endtable
endprimitive

primitive latch (q, g, d);
    output q;
    reg q;
    input g, d;
    table
    // g d : q : q+
       1 0 : ? : 0;
       1 1 : ? : 1;
       0 ? : ? : -;
       x 0 : 0 : -;
       x 1 : 1 : -;
    endtable
endprimitive

primitive dff (q, clk, d, clr);
    output q;
    reg q;
    input clk, d, clr;
    initial q = 1'b1;
    table
    // clk  d  clr : q : q+
       ?    ?  1   : ? : 0;
       (01) 0  0   : ? : 0;
       (01) 1  0   : ? : 1;
       (01) x  0   : ? : x;
       (01) ?  x   : ? : x;
       (01) ?  1   : ? : 0;
       (0x) ?  ?   : ? : x;
       (x1) ?  ?   : ? : x;
       (?0) ?  ?   : ? : -;
       (1x) ?  ?   : ? : -;
       ?    *  ?   : ? : -;
       ?    ?  (?0): ? : -;
       ?    ?  (?1): ? : 0;
       ?    ?  (?x): ? : x;
    endtable
endprimitive

module tb;
    logic d1, d0, s;
    wire o;
    mux2 u_mux (o, d1, d0, s);

    logic g = 1'b0, d = 1'b0;
    wire q_latch;
    latch u_latch (q_latch, g, d);

    // Not initialised, so that nothing changes until the initial state has been seen.
    logic clk, data, clr;
    wire q;
    dff u_dff (q, clk, data, clr);

    localparam logic [3:0] Values = 4'bzx10;

    initial begin
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 4; k++) begin
                    d1 = Values[i];
                    d0 = Values[j];
                    s = Values[k];
                    #1 $display("d1=%b d0=%b s=%b o=%b", d1, d0, s, o);
                end

        #1 $display("latch held q=%b", q_latch);
        g = 1'b1;
        #1 $display("latch open q=%b", q_latch);
        d = 1'b1;
        #1 $display("latch open q=%b", q_latch);
        g = 1'b0;
        #1 d = 1'b0;
        #1 $display("latch closed q=%b", q_latch);
        d = 1'b1;
        #1 g = 1'bx;
        #1 $display("latch gate x, data agrees q=%b", q_latch);
        d = 1'b0;
        #1 $display("latch gate x, data disagrees q=%b", q_latch);

        $display("dff initial q=%b", q);
        clr = 1'b0;
        #1 data = 1'b0;
        #1 clk = 1'b0;
        #1 $display("dff inputs known q=%b", q);
        clk = 1'b1;
        #1 $display("dff rose q=%b", q);
        data = 1'b1;
        #1 $display("dff data changed q=%b", q);
        clk = 1'b0;
        #1 clk = 1'b1;
        #1 $display("dff rose q=%b", q);
        clr = 1'b1;
        #1 $display("dff cleared q=%b", q);
        clk = 1'b0;
        #1 clk = 1'b1;
        #1 $display("dff rose while cleared q=%b", q);
        clr = 1'b0;
        #1 clk = 1'b0;
        #1 clk = 1'bx;
        #1 $display("dff clock to x q=%b", q);

        $display("PASS primitives");
        $finish;
    end
endmodule
