// SPDX-License-Identifier: GPL-3.0-or-later
//
// Random numbers. `$random` is IEEE 1364's generator, so the same seed draws the same sequence
// in every simulator that follows it, and this transcript is compared with Icarus Verilog's:
// with a seed variable it updates, and without, from the simulation's own seed. `$urandom` and
// `$urandom_range` draw from each process's own generator, which no two simulators share, so
// only their properties are checked.

module tb;
    integer seed;
    integer value;
    int unsigned first;
    int unsigned again;
    integer      seed_a;
    integer      seed_b;
    int failures = 0;

    initial begin
        seed = 0;
        repeat (4) begin
            value = $random(seed);
            $display("seeded %0d, seed now %0d", value, seed);
        end
        repeat (3) $display("global %0d", $random);

        // A seed given to $urandom restarts the generator, so the same seed draws the same.
        // Icarus Verilog takes the seed only from a variable.
        seed_a = 42;
        seed_b = 42;
        first = $urandom(seed_a);
        again = $urandom(seed_b);
        if (first != again) begin
            $display("FAIL: $urandom(42) drew %0d and then %0d", first, again);
            failures = failures + 1;
        end

        // Either bound may come first, and both are reachable.
        for (int draw = 0; draw < 200; draw++) begin
            value = $urandom_range(7, 3);
            if (value < 3 || value > 7) begin
                $display("FAIL: $urandom_range(7, 3) drew %0d", value);
                failures = failures + 1;
            end
            value = $urandom_range(2);
            if (value < 0 || value > 2) begin
                $display("FAIL: $urandom_range(2) drew %0d", value);
                failures = failures + 1;
            end
        end

        if (failures != 0) begin
            $display("FAIL random: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS random");
        $finish;
    end
endmodule
