// SPDX-License-Identifier: GPL-3.0-or-later
//
// Packages (IEEE 1800 section 26): constants, types, enumerations, functions and tasks used from
// modules, and package variables, of which there is one however many modules use them. Two
// modules count into one package counter, a package task counts into it from both, and a
// change to a package variable wakes a process that reads it.

package counters_pkg;
    localparam int Limit = 200;
    typedef logic [7:0] count_t;
    typedef enum logic [1:0] { IDLE, BUSY, DONE } state_e;

    int events = 0;
    count_t level = 8'd10;

    function automatic count_t saturating_add(count_t a, count_t b);
        logic [8:0] sum = a + b;
        return sum > Limit ? count_t'(Limit) : sum[7:0];
    endfunction

    task automatic record(int amount);
        events += amount;
    endtask
endpackage

module counter_user (input logic go);
    import counters_pkg::*;
    always @(posedge go) begin
        record(1);
        level = saturating_add(level, 8'd100);
    end
endmodule

module tb;
    import counters_pkg::*;

    logic   go = 1'b0;
    state_e state = IDLE;
    count_t seen;
    int     failures = 0;

    counter_user u_user (.go(go));

    // Reads a package variable, so a write to it from anywhere wakes this.
    always_comb seen = counters_pkg::level;

    initial begin
        if (level !== 8'd10 || events != 0) begin
            $display("FAIL: package variables should start at their initial values");
            failures = failures + 1;
        end

        #1 go = 1'b1;
        #1 go = 1'b0;
        record(10);
        #1 go = 1'b1;
        #1;
        if (level == count_t'(Limit)) state = DONE;
        else state = BUSY;

        $display("events=%0d level=%0d seen=%0d state=%0d", events, level, seen, state);
        if (events != 12) begin
            $display("FAIL: two modules counting into one package variable gave %0d", events);
            failures = failures + 1;
        end
        if (level !== 8'd200 || seen !== 8'd200 || state != DONE) begin
            $display("FAIL: level=%0d seen=%0d state=%0d", level, seen, state);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL packages: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS packages");
        $finish;
    end
endmodule
