// SPDX-License-Identifier: GPL-3.0-or-later
//
// Initialized output variables drive a connected net independently of its other
// drivers; neither an open output nor a converted connection loses its value.

module initialized #(parameter logic [7:0] START = 8'ha5) (
    output logic [7:0] state = START
);
    initial begin
        #4 state = 8'h3c;
        #4 state = 8'hf0;
    end
endmodule

module legacy(state);
    output reg [7:0] state = 8'h96;
endmodule

module nested(output wire [7:0] state);
    initialized child(state);
endmodule

module tb;
    wire [7:0] plain, shared, two_children, nested_value, legacy_value;
    wire [3:0] narrow;
    logic [7:0] other = 8'h00;

    initialized first(plain);
    initialized second(shared);
    initialized #(.START(8'h00)) conflict(two_children);
    initialized also_conflict(two_children);
    initialized short_output(narrow);
    initialized #(.START(8'h69)) unconnected();
    nested third(nested_value);
    legacy fourth(legacy_value);
    assign shared = other;

    initial begin
        #1;
        $display("initial %h %b %b %h %h %h %h", plain, shared, second.state,
                 nested_value, narrow, unconnected.state, legacy_value);
        if (plain !== 8'ha5 || shared !== 8'bx0x00x0x || two_children !== 8'bx0x00x0x
            || second.state !== 8'ha5 || conflict.state !== 8'h00
            || nested_value !== 8'ha5 || narrow !== 4'h5 || unconnected.state !== 8'h69
            || legacy_value !== 8'h96)
            $fatal(1, "initial output value or resolution");

        force shared = 8'h12;
        #1;
        $display("force %h %h", shared, second.state);
        if (shared !== 8'h12 || second.state !== 8'ha5) $fatal(1, "forced parent net");
        release shared;
        #1;
        $display("release %b %h", shared, second.state);
        if (shared !== 8'bx0x00x0x || second.state !== 8'ha5) $fatal(1, "released parent net");
        other = 8'hzz;
        #2;
        $display("updated %h %h %h", plain, shared, nested_value);
        if (plain !== 8'h3c || shared !== 8'h3c || two_children !== 8'h3c
            || nested_value !== 8'h3c)
            $fatal(1, "updated output value");

        force second.state = 8'h5a;
        #1;
        $display("force child %h %h", shared, second.state);
        if (shared !== 8'h5a || second.state !== 8'h5a) $fatal(1, "forced child variable");
        release second.state;
        #1;
        if (shared !== 8'h5a || second.state !== 8'h5a) $fatal(1, "released child variable");
        #2;
        $display("updated again %h %h", shared, second.state);
        if (shared !== 8'hf0 || second.state !== 8'hf0) $fatal(1, "output after release");
        $display("PASS port_initializers");
        $finish;
    end
endmodule
