// SPDX-License-Identifier: GPL-3.0-or-later
//
// A port connected to a signal of another width (IEEE 1800 section 23.3.3.7): the connection
// converts as an assignment would, an input from the signal and an output to it, extending
// by the signedness of what is assigned and truncating from the left.

module pass #(parameter int W = 4) (
    input  logic [W-1:0] i,
    output logic [W-1:0] o
);
    assign o = i;
endmodule

module pass_signed #(parameter int W = 8) (
    input  logic signed [W-1:0] i,
    output logic signed [W-1:0] o
);
    assign o = i;
endmodule

module tb;
    logic [5:0] wide_in = 6'b10_1101;
    logic [1:0] narrow_in = 2'b11;
    logic [5:0] wide_out;
    logic [1:0] narrow_out;
    logic [5:0] from_narrow;
    logic [1:0] from_wide;
    logic signed [3:0] negative = -4'sd3;
    logic signed [7:0] extended;
    logic [7:0] sign_kept;
    int failures = 0;

    // Wider and narrower signals on a 4-bit input, and on a 4-bit output.
    pass #(.W(4)) u_truncated_in (.i(wide_in), .o(narrow_out));
    pass #(.W(4)) u_extended_in (.i(narrow_in), .o(wide_out));
    pass #(.W(4)) u_narrow_signals (.i(narrow_in), .o(from_wide));
    pass #(.W(4)) u_wide_signals (.i(wide_in), .o(from_narrow));

    // A signed signal on a wider signed input extends its sign; the output keeps it.
    pass_signed #(.W(8)) u_signed (.i(negative), .o(extended));
    pass_signed #(.W(8)) u_signed_to_unsigned (.i(negative), .o(sign_kept));

    initial begin
        #1;
        $display("%b %b %b %b %b %b", narrow_out, wide_out, from_wide, from_narrow, extended,
                 sign_kept);
        if (narrow_out !== 2'b01 || wide_out !== 6'b00_0011 || from_wide !== 2'b11
            || from_narrow !== 6'b00_1101) begin
            $display("FAIL: an unsigned connection was converted wrongly");
            failures = failures + 1;
        end
        if (extended !== -8'sd3 || sign_kept !== 8'hfd) begin
            $display("FAIL: a signed connection lost its sign");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL port_widths: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS port_widths");
        $finish;
    end
endmodule
