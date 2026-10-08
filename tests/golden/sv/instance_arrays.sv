// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arrays of instances (IEEE 1800 section 23.3.3.5): each element is an instance of its own,
// named with its index, and a connection wider than a port is split across the elements, most
// significant element first.

module inverter (input logic a, output logic y);
    assign y = ~a;
endmodule

module adder (input logic [3:0] a, input logic [3:0] b, output logic [3:0] sum);
    assign sum = a + b;
endmodule

module tb;
    logic [3:0]  bits = 4'b0101;
    logic [3:0]  inverted;
    logic [11:0] left = 12'h123;
    logic [11:0] right = 12'h111;
    logic [11:0] sums;
    int failures = 0;

    inverter u_inverter [3:0] (.a(bits), .y(inverted));
    adder u_adder [2:0] (.a(left), .b(right), .sum(sums));

    initial begin
        #1;
        $display("inverted=%b sums=%h one=%b", inverted, sums, u_inverter[1].y);
        if (inverted !== 4'b1010 || sums !== 12'h234 || u_inverter[1].y !== 1'b1) begin
            $display("FAIL: an array of instances");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL instance_arrays: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS instance_arrays");
        $finish;
    end
endmodule
