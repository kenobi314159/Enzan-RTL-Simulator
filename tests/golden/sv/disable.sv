// SPDX-License-Identifier: GPL-3.0-or-later
//
// `disable` of a named block the statement is in, as Altera's models use it: a loop's body ended
// early, which goes on with the next iteration, as their memory-file readers do; a block holding
// a loop, which leaves the loop, as their PLLs search their counter settings; and the rest of a
// plain block skipped. Each is also in a function and in a task.

module tb;
    int failures = 0;

    task automatic check(int which, int got, int want);
        $display("check %0d: %0d", which, got);
        if (got != want) begin
            $display("FAIL: check %0d gave %0d, should be %0d", which, got, want);
            failures++;
        end
    endtask

    // Counts the odd numbers below `limit`, ending each even iteration's body early.
    function automatic int odd_below(int limit);
        int i = 0;
        int count = 0;
        while (i < limit) begin : body
            i++;
            if (i % 2 == 0) disable body;
            count++;
        end
        return count;
    endfunction

    // The first factor of `n` above 1, by a search the block around it ends. The loops' variables
    // are declared outside them: Icarus Verilog 13 crashes when a `disable` leaves a loop that
    // declares its own in an automatic function.
    function automatic int first_factor(int n);
        int found = n;
        int f, g;
        begin : search
            for (f = 2; f < n; f++) begin
                for (g = f; g <= n / f; g++) begin
                    if (f * g == n) begin
                        found = f;
                        disable search;
                    end
                end
            end
        end
        return found;
    endfunction

    int steps;

    // A task whose block ends partway, and what follows the block still runs.
    task automatic count_until(int stop, output int reached);
        reached = 0;
        begin : counting
            repeat (10) begin
                if (reached == stop) disable counting;
                reached++;
            end
            reached = -1;
        end
        reached = reached * 10;
    endtask

    initial begin
        int lines;
        int skipped;
        int reached;

        check(1, odd_below(9), 5);
        check(2, first_factor(91), 7);
        check(3, first_factor(13), 13);

        // A reader's loop: a comment line is skipped by ending the body, and an error ends the
        // loop by its condition, as the next test of it finds.
        lines = 0;
        skipped = 0;
        steps = 0;
        while (steps < 8) begin : reader
            steps++;
            if (steps == 3 || steps == 5) begin
                skipped++;
                disable reader;
            end
            if (steps == 6) begin
                steps = 100;
                disable reader;
            end
            lines++;
        end
        check(4, lines, 3);
        check(5, skipped, 2);
        check(6, steps, 100);

        // The rest of a plain block, after a wait.
        begin : timed
            #5;
            if ($time == 5) disable timed;
            failures++;
            $display("FAIL: the block went on after its disable");
        end
        check(7, int'($time), 5);

        count_until(4, reached);
        check(8, reached, 40);
        count_until(20, reached);
        check(9, reached, -10);

        if (failures == 0) $display("PASS disable");
        $finish;
    end
endmodule
