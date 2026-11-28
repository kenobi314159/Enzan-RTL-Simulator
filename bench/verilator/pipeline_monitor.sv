// SPDX-License-Identifier: GPL-3.0-or-later
//
// Validate the benchmark after its eight-stage pipeline has filled. Sampling on the falling edge
// observes settled register values, independent of a simulator's ordering at the rising edge.
module pipeline_monitor;
    bench #(.CYCLES(64)) dut ();

    always @(negedge dut.clk) begin
        if (dut.cycle_count >= 9)
            $display("state=%0d %08h %08h %08h %08h %08h %08h %08h %08h %08h", dut.cycle_count,
                     dut.seed, dut.s0, dut.s1, dut.s2, dut.s3, dut.s4, dut.s5, dut.s6, dut.s7);
    end
endmodule
