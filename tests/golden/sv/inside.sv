// SPDX-License-Identifier: GPL-3.0-or-later
//
// `inside` and `case ... inside` (IEEE 1800 sections 11.4.13 and 12.5.4): an item that is a
// value matches by `==?`, so its X and Z bits match anything, and an item `[low:high]` matches
// a value within its bounds. A set that cannot match and could, because of an unknown, is X.

module tb;
    logic [7:0] value;
    logic [3:0] result;
    int failures = 0;

    initial begin
        value = 8'd12;
        if (!(value inside {8'd3, [8'd10:8'd20]}) || (value inside {8'd3, [8'd13:8'd20]})) begin
            $display("FAIL: inside with a range");
            failures = failures + 1;
        end
        if (!(value inside {8'b0000_11xx})) begin
            $display("FAIL: an inside item's x bits should match anything");
            failures = failures + 1;
        end

        value = 8'bxxxx_0000;
        if ((value inside {8'd1, 8'd2}) !== 1'b0 || (value inside {8'h10}) !== 1'bx) begin
            $display("FAIL: inside with an unknown value, got %b and %b",
                     value inside {8'd1, 8'd2}, value inside {8'h10});
            failures = failures + 1;
        end

        value = 8'd42;
        result = 4'd0;
        case (value) inside
            [8'd0:8'd9]:     result = 4'd1;
            8'd40, [41:45]:  result = 4'd2;
            8'b1???_????:    result = 4'd3;
            default:         result = 4'd9;
        endcase
        if (result !== 4'd2) begin
            $display("FAIL: case inside chose %0d for 42", result);
            failures = failures + 1;
        end

        value = 8'd200;
        result = 4'd0;
        case (value) inside
            [8'd0:8'd9]:     result = 4'd1;
            8'b1???_????:    result = 4'd3;
            default:         result = 4'd9;
        endcase
        if (result !== 4'd3) begin
            $display("FAIL: case inside chose %0d for 200", result);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL inside: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS inside");
        $finish;
    end
endmodule
