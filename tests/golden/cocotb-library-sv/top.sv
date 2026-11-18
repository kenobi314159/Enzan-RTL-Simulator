// SPDX-License-Identifier: GPL-3.0-or-later

`include "expected.svh"

module top(output logic [3:0] q);
    counter u_counter (.q(q));

    initial begin
        #1;
        if (q != `EXPECTED) $fatal(1, "the input library's counter gave %0d", q);
    end
endmodule
