// SPDX-License-Identifier: GPL-3.0-or-later
//
// Run by random_stability.sh. `watched` prints what it draws from `$urandom` and `$random`.
// With Extra set, a second process draws as well. `watched`'s `$urandom` sequence must not
// change when it does, because each process has its own generator (IEEE 1800 section 18.14).

module tb #(parameter int Extra = 0);
    initial begin : watched
        repeat (3) begin
            #1 $display("urandom %0d", $urandom);
        end
        $display("random %0d", $random);
    end

    initial begin : other
        int unsigned discarded;
        if (Extra != 0) repeat (5) discarded = $urandom;
    end
endmodule
