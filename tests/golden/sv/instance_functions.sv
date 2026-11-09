// SPDX-License-Identifier: GPL-3.0-or-later
//
// Calls to the functions of another instance, `dev.has_m9k(family)`, whose static variables are
// that instance's (IEEE 1800 section 13.3.1), as Altera's models ask the `ALTERA_DEVICE_FAMILIES`
// they instantiate about the device family: a function that calls another of its module, and one
// whose static keeps what it was last given, which each instance keeps for itself and which a
// call from further up reaches too.

module families;
    function is_cyclone;
        input [8*20:1] device;
        reg found;
        begin
            if (device == "Cyclone 10 LP" || device == "Cyclone IV E") found = 1;
            else found = 0;
            is_cyclone = found;
        end
    endfunction

    function has_m9k;
        input [8*20:1] device;
        reg cyclone;
        begin
            cyclone = is_cyclone(device);
            has_m9k = cyclone;
        end
    endfunction

    // What the call before this one was given.
    function [7:0] last_given;
        input [7:0] value;
        reg [7:0] last;
        begin
            last_given = last;
            last = value;
        end
    endfunction
endmodule

module parent #(parameter [8*20:1] FAMILY = "Stratix") (
    output reg m9k,
    output reg [7:0] first,
    output reg [7:0] second
);
    families dev ();

    initial begin
        m9k = dev.has_m9k(FAMILY);
        first = dev.last_given(8'h11);
        second = dev.last_given(8'h22);
    end
endmodule

module tb;
    wire m9k_a, m9k_b;
    wire [7:0] first_a, second_a, first_b, second_b;
    reg [7:0] third_a;
    integer failures = 0;

    parent #(.FAMILY("Cyclone 10 LP")) a (m9k_a, first_a, second_a);
    parent b (m9k_b, first_b, second_b);

    initial begin
        #1;
        third_a = a.dev.last_given(8'h33);
        $display("a: m9k=%b first=%h second=%h third=%h", m9k_a, first_a, second_a, third_a);
        $display("b: m9k=%b first=%h second=%h", m9k_b, first_b, second_b);
        if (m9k_a !== 1'b1 || m9k_b !== 1'b0) failures = failures + 1;
        if (first_a !== 8'hxx || second_a !== 8'h11 || third_a !== 8'h22) failures = failures + 1;
        if (first_b !== 8'hxx || second_b !== 8'h11) failures = failures + 1;
        if (failures == 0) $display("PASS instance_functions");
        $finish;
    end
endmodule
