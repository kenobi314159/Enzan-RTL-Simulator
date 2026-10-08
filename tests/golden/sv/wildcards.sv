// SPDX-License-Identifier: GPL-3.0-or-later
//
// The comparisons that treat some bits as don't-cares (IEEE 1800 sections 11.4.6, 11.4.13 and
// 12.5): `==?` and `!=?`, where only the right operand's X and Z bits match anything; `casez`,
// where Z on either side does; `casex`, where X and Z on either side do; and `inside`, which
// compares by `==?` and by range.

module tb;
    logic [3:0] value;
    logic [3:0] result;
    int failures = 0;


    initial begin
        value = 4'b1010;
        if ((value ==? 4'b1x1z) !== 1'b1 || (value ==? 4'b0x1z) !== 1'b0
            || (value !=? 4'b0x1z) !== 1'b1) begin
            $display("FAIL: ==? or !=? on a known left operand");
            failures = failures + 1;
        end

        // The left operand's unknowns are not wildcards.
        value = 4'b1x10;
        if ((value ==? 4'b1010) !== 1'bx || (value ==? 4'b0010) !== 1'b0) begin
            $display("FAIL: ==? with an unknown on the left, got %b and %b", value ==? 4'b1010,
                     value ==? 4'b0010);
            failures = failures + 1;
        end

        // casez: ? and z in an item are don't-cares, and so is z in the selector.
        value = 4'b0110;
        result = 4'd0;
        casez (value)
            4'b1???: result = 4'd1;
            4'b01??: result = 4'd2;
            default: result = 4'd9;
        endcase
        $display("casez %b -> %0d", value, result);
        if (result !== 4'd2) begin
            $display("FAIL: casez chose %0d", result);
            failures = failures + 1;
        end

        value = 4'bz001;
        result = 4'd0;
        casez (value)
            4'b1001: result = 4'd1;
            4'b0011: result = 4'd2;
            default: result = 4'd9;
        endcase
        $display("casez %b -> %0d", value, result);
        if (result !== 4'd1) begin
            $display("FAIL: casez with z in the selector chose %0d", result);
            failures = failures + 1;
        end

        // casez compares x as a value; casex makes it a don't-care too.
        value = 4'b10x1;
        result = 4'd0;
        casez (value)
            4'b1001: result = 4'd1;
            default: result = 4'd9;
        endcase
        $display("casez %b -> %0d", value, result);
        if (result !== 4'd9) begin
            $display("FAIL: casez matched an x in the selector against a 0");
            failures = failures + 1;
        end

        result = 4'd0;
        casex (value)
            4'b1001: result = 4'd1;
            default: result = 4'd9;
        endcase
        $display("casex %b -> %0d", value, result);
        if (result !== 4'd1) begin
            $display("FAIL: casex chose %0d", result);
            failures = failures + 1;
        end


        if (failures != 0) begin
            $display("FAIL wildcards: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS wildcards");
        $finish;
    end
endmodule
