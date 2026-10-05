// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interfaces (IEEE 1800 section 25): an interface with signals and logic of its own, modules
// reading and writing it through ports with modports, an interface passed down through a module
// to the one that uses it, and two instances of one module on two interfaces, which share one
// definition and must each reach their own.

interface stream_if (input logic clk);
    logic [7:0] data;
    logic       valid;
    logic       ready;
    logic [7:0] beats;

    // Logic of the interface's own.
    always_ff @(posedge clk) if (valid && ready) beats <= beats + 8'd1;
    initial beats = 8'd0;

    modport source (output data, output valid, input ready);
    modport sink (input data, input valid, output ready);
endinterface

module producer #(parameter logic [7:0] START = 8'd0) (input logic clk, stream_if.source out);
    logic [7:0] next = START;
    always_ff @(posedge clk) begin
        out.valid <= 1'b1;
        if (out.ready) begin
            out.data <= next;
            next <= next + 8'd1;
        end
    end
endmodule

module consumer (input logic clk, stream_if.sink in, output logic [7:0] last);
    assign in.ready = 1'b1;
    always_ff @(posedge clk) if (in.valid) last <= in.data;
endmodule

// Passes its interface port straight down to the module that uses it.
module wrapper (input logic clk, stream_if.sink in, output logic [7:0] last);
    consumer u_consumer (.clk(clk), .in(in), .last(last));
endmodule

module tb;
    logic       clk = 1'b0;
    logic [7:0] last_a;
    logic [7:0] last_b;
    int failures = 0;

    stream_if a (.clk(clk));
    stream_if b (.clk(clk));

    producer #(.START(8'd10)) u_producer_a (.clk(clk), .out(a));
    producer #(.START(8'd10)) u_producer_b (.clk(clk), .out(b));
    consumer u_consumer_a (.clk(clk), .in(a), .last(last_a));
    wrapper u_wrapper_b (.clk(clk), .in(b), .last(last_b));

    always #5 clk = ~clk;

    initial begin
        b.valid = 1'b0;
        #61;
        $display("a.data=%0d a.beats=%0d last_a=%0d last_b=%0d", a.data, a.beats, last_a,
                 last_b);
        if (a.data !== 8'd15 || last_a !== 8'd14 || last_b !== 8'd14) begin
            $display("FAIL: data through the interfaces");
            failures = failures + 1;
        end
        if (a.beats !== 8'd5 || b.beats !== a.beats) begin
            $display("FAIL: the interface's own logic counted %0d and %0d", a.beats, b.beats);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL interfaces: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS interfaces");
        $finish;
    end
endmodule
