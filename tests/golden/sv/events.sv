// SPDX-License-Identifier: GPL-3.0-or-later
//
// Event controls on selects and event lists (IEEE 1800 section 9.4.2): an edge on one bit of a
// vector, which is the edge of that bit alone; a change on a part select, which only a change
// to that part triggers; and an event list inside a body, which resumes once on whichever of its
// events happens first, even when several happen in the same time step.

module tb;
    logic [1:0] clocks = 2'b00;
    logic [7:0] bus = 8'h00;
    logic       a = 1'b0;
    logic       b = 1'b1;
    int second_clock_edges = 0;
    int high_changes = 0;
    int list_resumes = 0;
    int failures = 0;

    always @(posedge clocks[1]) second_clock_edges = second_clock_edges + 1;
    always @(bus[7:4]) high_changes = high_changes + 1;

    initial forever begin
        @(posedge a or negedge b);
        list_resumes = list_resumes + 1;
    end

    initial begin
        #1 clocks = 2'b01;
        #1 clocks = 2'b11;
        #1 clocks = 2'b10;
        #1 clocks = 2'b00;
        #1 clocks = 2'b10;

        #1 bus = 8'h0f;
        #1 bus = 8'h1f;
        #1 bus = 8'h10;
        #1 bus = 8'h30;

        #1 a = 1'b1;
        #1 b = 1'b0;
        #1 begin a = 1'b0; b = 1'b1; end
        #1 begin a = 1'b1; b = 1'b0; end

        #1;
        $display("second_clock_edges=%0d high_changes=%0d list_resumes=%0d", second_clock_edges,
                 high_changes, list_resumes);
        if (second_clock_edges != 2 || high_changes != 2 || list_resumes != 3) begin
            $display("FAIL: event controls on selects and lists");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL events: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS events");
        $finish;
    end
endmodule
