// SPDX-License-Identifier: GPL-3.0-or-later
//
// The router of tests/golden/sv/router.sv, an arbiter over three channels each holding a state
// machine, run for a fixed number of cycles with faults and starvation injected on a fixed
// schedule, so that a transfer's failing paths are measured beside its clean one.
//
// Where pipeline.sv is data flowing through registers, this is control: enumerations, case
// statements, an asynchronous reset on every register, and a hierarchy three levels deep with one
// module at three parameterisations. The cycle count is fixed in the source for the reason
// pipeline.sv gives, and the router is given more transfers than the run can issue, so that it
// never comes to rest.

module bench #(
    parameter int CYCLES = 400000
);
    logic clk = 1'b0;
    logic rst_n = 1'b0;
    int   cycle_count = 0;

    logic [15:0] issued;
    logic [15:0] completed;
    logic [15:0] failures;

    // Each kind of failure is held for a stretch of cycles at a time, at a period of its own.
    logic starve2;
    logic fault1;
    logic fault0;

    assign starve2 = (cycle_count[11:9] == 3'd5);
    assign fault1  = (cycle_count[12:8] == 5'd17);
    assign fault0  = (cycle_count[13:6] == 8'd200);

    router #(.TOTAL(16'hFFFF)) u_sys (
        .clk(clk),
        .rst_n(rst_n),
        .enable(1'b1),
        .fault0(fault0),
        .fault1(fault1),
        .fault2(1'b0),
        .starve0(1'b0),
        .starve1(1'b0),
        .starve2(starve2),
        .issued(issued),
        .completed(completed),
        .failures(failures),
        .sent0(),
        .sent1(),
        .sent2(),
        .lost0(),
        .lost1(),
        .lost2(),
        .state0(),
        .state1(),
        .state2(),
        .id0(),
        .id1(),
        .id2(),
        .idle()
    );

    always #5 clk = ~clk;

    always_ff @(posedge clk) begin
        cycle_count <= cycle_count + 1;
        if (cycle_count == 2) rst_n <= 1'b1;
    end

    initial begin
        while (cycle_count < CYCLES)
            @(posedge clk);

        // Printed so that the harness can confirm the run did the work it was asked for, and
        // so that a change which accidentally optimises the design away is visible.
        $display("cycles=%0d issued=%0d completed=%0d failures=%0d", cycle_count, issued,
                 completed, failures);
        $finish;
    end
endmodule
