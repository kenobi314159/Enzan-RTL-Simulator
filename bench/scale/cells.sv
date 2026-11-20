// SPDX-License-Identifier: GPL-3.0-or-later
// A synthetic active network of explicitly instantiated six-input LUTs.
`ifdef SYNTHESIS
(* blackbox *)
`endif
module LUT6 #(parameter [63:0] INIT = 64'h6c93936c96696996)
    (input I0, I1, I2, I3, I4, I5, output O);
`ifndef SYNTHESIS
    assign O = INIT[{I5,I4,I3,I2,I1,I0}];
`endif
endmodule

module network #(parameter integer WIDTH = 1024)
    (input logic clk, output logic [WIDTH-1:0] state);
    logic phase = 0;
    wire [WIDTH-1:0] next_state;
    initial state = {WIDTH/32{32'hd3a5819b}};
    for (genvar i = 0; i < WIDTH; i++) begin : cells
        (* keep *) LUT6 lut(.I0(state[i]), .I1(state[(i+1)%WIDTH]),
                          .I2(state[(i+2)%WIDTH]), .I3(state[(i+3)%WIDTH]),
                          .I4(state[(i+4)%WIDTH]), .I5(phase),
                          .O(next_state[i]));
    end
    always_ff @(posedge clk) state <= next_state;
    always_ff @(negedge clk) phase <= ~phase;
endmodule

`ifndef SYNTHESIS
module bench #(parameter integer WIDTH = 1024, parameter integer TRACE = 0);
    logic clk = 0;
    wire [WIDTH-1:0] state;
    network #(.WIDTH(WIDTH)) dut(.clk(clk), .state(state));
    always #5 clk = ~clk;
    initial begin
        repeat (20) begin
            @(negedge clk);
            if (TRACE != 0) $display("state=%08x", state[31:0]);
        end
        $display("cells=%0d cycles=20 ones=%0d", WIDTH, $countones(state));
        $finish;
    end
endmodule
`endif
