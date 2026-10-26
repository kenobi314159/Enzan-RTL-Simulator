// SPDX-License-Identifier: GPL-3.0-or-later
//
// A SystemVerilog testbench around a VHDL unit (shapes.vhd) whose record port is a packed struct
// here, field for field, whose array of vectors is an unpacked array, element for element, and
// whose bus each side drives and releases in turn.
`timescale 1ns / 1ps

module tb;
    typedef struct packed {
        logic [3:0] red;
        logic [3:0] green;
        logic [3:0] blue;
        logic valid;
    } pixel_t;

    pixel_t inp, outp;
    logic [7:0] lane [0:3];
    logic [9:0] sum;
    logic drive;
    logic [3:0] seen;
    logic [3:0] mine;
    wire [3:0] data;
    assign data = mine;

    Shapes dut (.Inp(inp), .Outp(outp), .Lane(lane), .Sum(sum), .Drive(drive), .Data(data),
                .Seen(seen));

    initial begin
        inp = {4'h1, 4'h2, 4'h3, 1'b1};
        lane[0] = 8'd10;
        lane[1] = 8'd200;
        lane[2] = 8'd255;
        lane[3] = 8'd7;
        drive = 1'b0;
        mine = 4'bzzzz;
        #1 $display("%0d: out %h %h %h %b, sum %0d", $time, outp.red, outp.green, outp.blue,
                    outp.valid, sum);
        inp.valid = 1'b0;
        lane[2] = 8'd1;
        #1 $display("%0d: out %h %h %h %b, sum %0d", $time, outp.red, outp.green, outp.blue,
                    outp.valid, sum);

        // The unit drives the bus, then the testbench, then both, then neither.
        drive = 1'b1;
        #1 $display("%0d: driven by the unit, %b reads %b", $time, data, seen);
        drive = 1'b0;
        mine = 4'b0110;
        #1 $display("%0d: driven by the testbench, %b reads %b", $time, data, seen);
        drive = 1'b1;
        #1 $display("%0d: driven by both, %b reads %b", $time, data, seen);
        drive = 1'b0;
        mine = 4'bzzzz;
        #1 $display("%0d: released, %b reads %b", $time, data, seen);
        if (outp.red == 4'h3 && outp.green == 4'h2 && outp.blue == 4'h1 && outp.valid
            && sum == 10'd218
            && seen === 4'bzzzz)
            $display("PASS shapes");
        else $display("FAIL shapes");
        $finish;
    end
endmodule
