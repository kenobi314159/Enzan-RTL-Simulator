// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bus shared through inout ports: two modules each drive it or release it, and each reads back
// what it resolves to, as the testbench does.

module talker #(
    parameter logic [3:0] VALUE = 4'b0000
) (
    input logic speak,
    inout wire [3:0] bus,
    output logic [3:0] heard
);
    assign bus = speak ? VALUE : 4'bz;
    assign heard = bus;
endmodule

module tb;
    logic speak_a = 1'b0;
    logic speak_b = 1'b0;
    wire [3:0] bus;
    logic [3:0] heard_a, heard_b;

    talker #(.VALUE(4'b1100)) a (.speak(speak_a), .bus(bus), .heard(heard_a));
    talker #(.VALUE(4'b1010)) b (.speak(speak_b), .bus(bus), .heard(heard_b));

    // Released, a speaking, both, b, and released again.
    task automatic expect_bus(input int step, input logic [3:0] wanted);
        #1;
        $display("%0d: bus %b, a hears %b, b hears %b", step, bus, heard_a, heard_b);
        if (bus !== wanted || heard_a !== wanted || heard_b !== wanted)
            $display("FAIL inout: step %0d", step);
    endtask

    initial begin
        expect_bus(1, 4'bzzzz);
        speak_a = 1'b1;
        expect_bus(2, 4'b1100);
        speak_b = 1'b1;
        expect_bus(3, 4'b1xx0);
        speak_a = 1'b0;
        expect_bus(4, 4'b1010);
        speak_b = 1'b0;
        expect_bus(5, 4'bzzzz);
        $display("PASS inout");
        $finish;
    end
endmodule
