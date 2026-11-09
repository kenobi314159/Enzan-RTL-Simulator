// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built-in gates (IEEE 1800 section 28): each kind over every pair of the four values, a `z`
// input reading as `x`; the tristate buffers with their control at each value, an unknown one
// driving `x`; an array of gates; a gate's delay; and a pad as an I/O buffer drives it, with a
// pull-up that holds it once released and a weak buffer that keeps it, which a strong driver beats.

module tb;
    logic a, b;
    wire y_and, y_or, y_xor, y_nand, y_nor, y_xnor, y_buf, y_not;
    and g_and (y_and, a, b);
    or g_or (y_or, a, b);
    xor g_xor (y_xor, a, b);
    nand g_nand (y_nand, a, b);
    nor g_nor (y_nor, a, b);
    xnor g_xnor (y_xnor, a, b);
    buf g_buf (y_buf, a);
    not g_not (y_not, a);

    wire t_bufif0, t_bufif1, t_notif0, t_notif1;
    bufif0 g_bufif0 (t_bufif0, a, b);
    bufif1 g_bufif1 (t_bufif1, a, b);
    notif0 g_notif0 (t_notif0, a, b);
    notif1 g_notif1 (t_notif1, a, b);

    logic [3:0] word;
    wire [3:0] inverted;
    not g_array[3:0] (inverted, word);

    logic slow_in = 1'b0;
    wire slow_out;
    buf #3 g_slow (slow_out, slow_in);

    // A pad: an output buffer enabled low, a pull-up, and a weak keeper of its own value.
    logic out_value = 1'b0, out_disable = 1'b1;
    wire pad;
    bufif0 g_pad (pad, out_value, out_disable);
    pullup g_pullup (pad);

    logic keep_value = 1'b0, keep_enable = 1'b0;
    wire kept;
    bufif1 (weak1, weak0) g_keeper (kept, keep_value, keep_enable);
    bufif1 g_strong (kept, 1'b1, ~keep_enable);

    localparam logic [3:0] Values = 4'bzx10;

    initial begin
        for (int i = 0; i < 4; i++) begin
            for (int j = 0; j < 4; j++) begin
                a = Values[i];
                b = Values[j];
                #1 $display("a=%b b=%b and=%b or=%b xor=%b nand=%b nor=%b xnor=%b buf=%b not=%b",
                            a, b, y_and, y_or, y_xor, y_nand, y_nor, y_xnor, y_buf, y_not);
                $display("a=%b b=%b bufif0=%b bufif1=%b notif0=%b notif1=%b", a, b, t_bufif0,
                         t_bufif1, t_notif0, t_notif1);
            end
        end

        word = 4'b10zx;
        #1 $display("word=%b inverted=%b", word, inverted);

        slow_in = 1'b1;
        #2 $display("t=2 slow_out=%b", slow_out);
        #2 $display("t=4 slow_out=%b", slow_out);

        #1 $display("released pad=%b", pad);
        out_disable = 1'b0;
        #1 $display("driven pad=%b", pad);

        #1 $display("strong kept=%b", kept);
        keep_enable = 1'b1;
        #1 $display("weak kept=%b", kept);

        $display("PASS gates");
        $finish;
    end
endmodule
