// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's multi-port LUT RAMs RAM32M, of two-bit words, and RAM64M, against memories of the
// testbench's own: their initial contents through each of the four read ports, writes through
// port D's address on enabled clock edges, and reads at addresses that change between edges. The
// models' paths run from bits of the addresses and to bits of the outputs; every check is of a
// settled value, 2 ns after what it depends on changes, their longest path being 1163 ps.
//
// Icarus: without -gspecify, since it applies a path from or to a bit of a vector to all of it.

`timescale 1ns / 1ps

module tb_lutram_ports;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    localparam logic [63:0] INIT_A = 64'h0123_4567_89ab_cdef, INIT_B = 64'hfedc_ba98_7654_3210;
    localparam logic [63:0] INIT_C = 64'h5a5a_a5a5_3c3c_c3c3, INIT_D = 64'h0f0f_f0f0_6969_9696;

    logic we = 1'b0;
    logic [5:0] addr_a = 6'd0, addr_b = 6'd0, addr_c = 6'd0, addr_d = 6'd0;
    logic [1:0] di_a = 2'd0, di_b = 2'd0, di_c = 2'd0, di_d = 2'd0;
    wire [1:0] do32_a, do32_b, do32_c, do32_d;
    wire do64_a, do64_b, do64_c, do64_d;
    RAM32M #(.INIT_A(INIT_A), .INIT_B(INIT_B), .INIT_C(INIT_C), .INIT_D(INIT_D)) u32 (
        .DOA(do32_a), .DOB(do32_b), .DOC(do32_c), .DOD(do32_d),
        .ADDRA(addr_a[4:0]), .ADDRB(addr_b[4:0]), .ADDRC(addr_c[4:0]), .ADDRD(addr_d[4:0]),
        .DIA(di_a), .DIB(di_b), .DIC(di_c), .DID(di_d), .WCLK(clk), .WE(we));
    RAM64M #(.INIT_A(INIT_A), .INIT_B(INIT_B), .INIT_C(INIT_C), .INIT_D(INIT_D)) u64 (
        .DOA(do64_a), .DOB(do64_b), .DOC(do64_c), .DOD(do64_d),
        .ADDRA(addr_a), .ADDRB(addr_b), .ADDRC(addr_c), .ADDRD(addr_d),
        .DIA(di_a[0]), .DIB(di_b[0]), .DIC(di_c[0]), .DID(di_d[0]), .WCLK(clk), .WE(we));

    logic [63:0] m32_a = INIT_A, m32_b = INIT_B, m32_c = INIT_C, m32_d = INIT_D;
    logic [63:0] m64_a = INIT_A, m64_b = INIT_B, m64_c = INIT_C, m64_d = INIT_D;
    always @(posedge clk)
        if (we) begin
            m32_a[2 * addr_d[4:0] +: 2] <= di_a;
            m32_b[2 * addr_d[4:0] +: 2] <= di_b;
            m32_c[2 * addr_d[4:0] +: 2] <= di_c;
            m32_d[2 * addr_d[4:0] +: 2] <= di_d;
            m64_a[addr_d] <= di_a[0];
            m64_b[addr_d] <= di_b[0];
            m64_c[addr_d] <= di_c[0];
            m64_d[addr_d] <= di_d[0];
        end

    int failures = 0;
    task automatic compare(int which);
        logic [11:0] got, want;
        got = {do32_a, do32_b, do32_c, do32_d, do64_a, do64_b, do64_c, do64_d};
        want = {m32_a[2 * addr_a[4:0] +: 2], m32_b[2 * addr_b[4:0] +: 2],
                m32_c[2 * addr_c[4:0] +: 2], m32_d[2 * addr_d[4:0] +: 2],
                m64_a[addr_a], m64_b[addr_b], m64_c[addr_c], m64_d[addr_d]};
        if (got !== want) begin
            $display("FAIL: check %0d at %h %h %h %h gave %b, should be %b", which, addr_a, addr_b,
                     addr_c, addr_d, got, want);
            failures = failures + 1;
        end
    endtask

    // The clock rises at 5, 15 and so on, and falls at 10, 20 and so on.
    initial begin
        // The initial contents, nothing written, each port at an address of its own.
        for (int n = 0; n < 64; n = n + 1) begin
            addr_a = 6'(n);
            addr_b = 6'(63 - n);
            addr_c = 6'(n * 5);
            addr_d = 6'(n * 7 + 3);
            #2 compare(n);
        end

        // Writes, and reads at every port, the inputs changing 2 ns after a falling edge.
        for (int step = 0; step < 80; step = step + 1) begin
            @(negedge clk);
            #2;
            we = step % 5 != 2;
            {di_a, di_b, di_c, di_d} = 8'(step * 37 + 5);
            addr_d = 6'(step * 11);
            addr_a = step % 2 == 0 ? addr_d : 6'(step * 3);
            addr_b = 6'(step * 13 + 1);
            addr_c = step % 3 == 0 ? addr_d : 6'(63 - step);
            #2 compare(1000 + step);
            @(posedge clk);
            #2 compare(2000 + step);
        end

        if (failures == 0) $display("PASS tb_lutram_ports");
        $finish;
    end
endmodule
