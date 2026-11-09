// SPDX-License-Identifier: GPL-3.0-or-later
//
// Yosys's dual-port LUT RAMs RAM32X1D, RAM64X1D and RAM128X1D against memories of the
// testbench's own: their initial contents through both ports, writes on enabled clock edges,
// reads at addresses that change between edges, and the delays of the paths whose conditions
// name a parameter, 1153 ps from the clock to both ports when a write changes what they read,
// and from the address 642 ps for A0 and 127 ps for DPRA5. RAM128X1D's paths, up to 1545 ps, are
// not Icarus Verilog's, the model leaving them out for it, so only its settled values are checked,
// 2 ns after what they depend on changes.

`timescale 1ns / 1ps

module tb_lutram;
    logic clk = 1'b0;
    always #5 clk = ~clk;

    localparam logic [31:0] INIT32 = 32'hdead_beef;
    localparam logic [63:0] INIT64 = 64'h0123_4567_89ab_cdef;
    localparam logic [127:0] INIT128 = 128'h0f1e_2d3c_4b5a_6978_8796_a5b4_c3d2_e1f0;

    logic we = 1'b0, d = 1'b0;
    logic [6:0] a = 7'd0, dpra = 7'd0;
    wire spo32, dpo32, spo64, dpo64, spo128, dpo128;
    RAM32X1D #(.INIT(INIT32)) u32 (
        .SPO(spo32), .DPO(dpo32), .D(d), .WCLK(clk), .WE(we),
        .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .A4(a[4]),
        .DPRA0(dpra[0]), .DPRA1(dpra[1]), .DPRA2(dpra[2]), .DPRA3(dpra[3]), .DPRA4(dpra[4]));
    RAM64X1D #(.INIT(INIT64)) u64 (
        .SPO(spo64), .DPO(dpo64), .D(d), .WCLK(clk), .WE(we),
        .A0(a[0]), .A1(a[1]), .A2(a[2]), .A3(a[3]), .A4(a[4]), .A5(a[5]),
        .DPRA0(dpra[0]), .DPRA1(dpra[1]), .DPRA2(dpra[2]), .DPRA3(dpra[3]), .DPRA4(dpra[4]),
        .DPRA5(dpra[5]));
    RAM128X1D #(.INIT(INIT128)) u128 (
        .SPO(spo128), .DPO(dpo128), .D(d), .WCLK(clk), .WE(we), .A(a), .DPRA(dpra));

    logic [31:0] mem32 = INIT32;
    logic [63:0] mem64 = INIT64;
    logic [127:0] mem128 = INIT128;
    always @(posedge clk)
        if (we) begin
            mem32[a[4:0]] <= d;
            mem64[a[5:0]] <= d;
            mem128[a] <= d;
        end

    int failures = 0;
    task automatic compare(int which);
        logic [5:0] got, want;
        got = {spo32, dpo32, spo64, dpo64, spo128, dpo128};
        want = {mem32[a[4:0]], mem32[dpra[4:0]], mem64[a[5:0]], mem64[dpra[5:0]], mem128[a],
                mem128[dpra]};
        if (got !== want) begin
            $display("FAIL: check %0d at a=%0d dpra=%0d gave %b, should be %b", which, a, dpra,
                     got, want);
            failures = failures + 1;
        end
    endtask
    task automatic expect2(int which, logic [1:0] got, logic [1:0] want);
        $display("%.3f: check %0d %b", $realtime, which, got);
        if (got !== want) begin
            $display("FAIL: check %0d gave %b, should be %b", which, got, want);
            failures = failures + 1;
        end
    endtask

    // The clock rises at 5, 15 and so on, and falls at 10, 20 and so on.
    initial begin
        // The initial contents, nothing written.
        for (int n = 0; n < 128; n = n + 1) begin
            a = n[6:0];
            dpra = 7'(127 - n);
            #2 compare(n);
        end

        // Writes, and reads at both ports, the inputs changing 2 ns after a falling edge.
        for (int step = 0; step < 60; step = step + 1) begin
            @(negedge clk);
            #2;
            we = step % 4 != 3;
            d = step % 3 == 0;
            a = 7'(step * 37);
            dpra = step % 2 == 0 ? 7'(step * 37) : 7'(step * 11);
            #2 compare(1000 + step);
            @(posedge clk);
            #2 compare(2000 + step);
        end

        // From the clock: a write to the address both ports read, of the other value.
        @(negedge clk);
        #2;
        we = 1'b1;
        a = 7'd0;
        dpra = 7'd0;
        d = ~mem32[0];
        @(posedge clk);
        #1.151 expect2(1, {spo32, dpo32}, {2{~d}});
        #0.004 expect2(2, {spo32, dpo32}, {2{d}});

        // From the address: A0 642 ps, after making address 1 differ from address 0.
        @(negedge clk);
        #2;
        a = 7'd1;
        d = ~mem32[0];
        @(posedge clk);
        #2;
        we = 1'b0;
        a = 7'd0;
        #0.640 expect2(3, {spo32, spo32}, {2{mem32[1]}});
        #0.004 expect2(4, {spo32, spo32}, {2{mem32[0]}});

        // From DPRA5 127 ps, to an address of the upper half holding the other value: the
        // other bits that change with it have longer paths.
        dpra = 7'd0;
        #2 for (int k = 32; k < 64; k = k + 1) begin
            if (mem64[k] != mem64[0] && dpra == 7'd0) dpra = 7'(k);
        end
        #0.125 expect2(5, {dpo64, dpo64}, {2{mem64[0]}});
        #0.004 expect2(6, {dpo64, dpo64}, {2{~mem64[0]}});

        if (failures == 0) $display("PASS tb_lutram");
        $finish;
    end
endmodule
