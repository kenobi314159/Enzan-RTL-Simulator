// SPDX-License-Identifier: GPL-3.0-or-later
//
// A type parameter handed down through a second level of hierarchy. Each specialisation of the
// inner module must follow the type its outer one was given, not share one module because the
// type reached it under the same parameter name. The two struct types here are equally wide,
// so sharing would not even change a width; it would read the fields in the wrong places.

module inner #(parameter type data_t = logic) (
    input  data_t d,
    output logic [7:0] low_field
);
    assign low_field = d.low;
endmodule

module outer #(parameter type data_t = logic) (
    input  data_t d,
    output logic [7:0] low_field
);
    inner #(.data_t(data_t)) u_inner (.d, .low_field);
endmodule

module tb;
    typedef struct packed {
        logic [15:0] high;
        logic [7:0]  low;
    } wide_high_t;

    typedef struct packed {
        logic [7:0]  low;
        logic [15:0] high;
    } wide_low_t;

    wide_high_t a;
    wide_low_t  b;
    logic [7:0] a_low;
    logic [7:0] b_low;
    int failures = 0;

    outer #(.data_t(wide_high_t)) u_a (.d(a), .low_field(a_low));
    outer #(.data_t(wide_low_t))  u_b (.d(b), .low_field(b_low));

    initial begin
        a = '{high: 16'h1234, low: 8'h56};
        b = '{low: 8'h9a, high: 16'hbcde};
        #1;
        $display("a.low=%h b.low=%h", a_low, b_low);
        if (a_low !== 8'h56 || b_low !== 8'h9a) begin
            $display("FAIL: the inner modules read the fields of the wrong struct");
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL type_parameters: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS type_parameters");
        $finish;
    end
endmodule
