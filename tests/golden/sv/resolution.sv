// SPDX-License-Identifier: GPL-3.0-or-later
//
// Nets with more than one driver, resolved per bit as DESIGN_GUIDE.md section 3.7 and IEEE
// 1800 tables 6-2 to 6-4 define: a tristate bus whose drivers take turns, contend and let go;
// wired-AND and wired-OR nets; pulled nets that a driver leaves at z; a supply net no strong
// driver can move; and two instances whose outputs overlap on one bus.

module tristate (
    input  logic       enable,
    input  logic [3:0] value,
    output wire  [3:0] bus
);
    assign bus = enable ? value : 4'bzzzz;
endmodule

module tb;
    logic       enable_a;
    logic       enable_b;
    logic [3:0] value_a;
    logic [3:0] value_b;
    wire  [3:0] bus;

    assign bus = enable_a ? value_a : 4'bzzzz;
    assign bus = enable_b ? value_b : 4'bzzzz;

    // Overlapping in bits 3 and 2 only; bits 5 and 4 and bits 1 and 0 have one driver each.
    wire [5:0] shared;
    tristate u_low  (.enable(enable_a), .value(value_a), .bus(shared[3:0]));
    tristate u_high (.enable(enable_b), .value(value_b), .bus(shared[5:2]));

    logic [1:0] p;
    logic [1:0] q;
    wand  [1:0] anded;
    wor   [1:0] ored;
    assign anded = p;
    assign anded = q;
    assign ored  = p;
    assign ored  = q;

    logic drive;
    tri0  down;
    tri1  up;
    supply1 power;
    assign down  = drive;
    assign up    = drive;
    assign power = drive;

    int failures = 0;


    initial begin
        // Nobody drives the bus.
        enable_a = 1'b0;
        enable_b = 1'b0;
        value_a  = 4'b1010;
        value_b  = 4'b0110;
        #1;
        $display("t=%0d bus=%b shared=%b", $time, bus, shared);
        if (bus !== 4'bzzzz) begin
            $display("FAIL: an undriven bus should be z, got %b", bus);
            failures = failures + 1;
        end

        // One driver at a time.
        enable_a = 1'b1;
        #1;
        $display("t=%0d bus=%b shared=%b", $time, bus, shared);
        if (bus !== 4'b1010 || shared !== 6'bzz1010) begin
            $display("FAIL: with a alone, bus is %b and shared is %b", bus, shared);
            failures = failures + 1;
        end

        enable_a = 1'b0;
        enable_b = 1'b1;
        #1;
        $display("t=%0d bus=%b shared=%b", $time, bus, shared);
        if (bus !== 4'b0110 || shared !== 6'b0110zz) begin
            $display("FAIL: with b alone, bus is %b and shared is %b", bus, shared);
            failures = failures + 1;
        end

        // Both: the bits they agree on keep their value, the rest are X. On `shared` they overlap
        // only in bits 3 and 2, where they happen to agree.
        enable_a = 1'b1;
        #1;
        $display("t=%0d bus=%b shared=%b", $time, bus, shared);
        if (bus !== 4'bxx10 || shared !== 6'b011010) begin
            $display("FAIL: with both, bus is %b and shared is %b", bus, shared);
            failures = failures + 1;
        end

        // Let go again.
        enable_a = 1'b0;
        enable_b = 1'b0;
        #1;
        if (bus !== 4'bzzzz) begin
            $display("FAIL: a released bus should return to z, got %b", bus);
            failures = failures + 1;
        end

        // Wired AND and OR: 0, respectively 1, wins; z yields.
        p = 2'b10;
        q = 2'b0z;
        #1;
        $display("t=%0d anded=%b ored=%b", $time, anded, ored);
        if (anded !== 2'b00 || ored !== 2'b10) begin
            $display("FAIL: wand is %b and wor is %b", anded, ored);
            failures = failures + 1;
        end
        p = 2'bx1;
        q = 2'b1z;
        #1;
        $display("t=%0d anded=%b ored=%b", $time, anded, ored);
        if (anded !== 2'bx1 || ored !== 2'b11) begin
            $display("FAIL: wand is %b and wor is %b", anded, ored);
            failures = failures + 1;
        end

        // A pulled net takes its pull where its driver is z, and a supply net ignores it.
        drive = 1'bz;
        #1;
        $display("t=%0d tri0=%b tri1=%b supply1=%b", $time, down, up, power);
        if (down !== 1'b0 || up !== 1'b1 || power !== 1'b1) begin
            $display("FAIL: driven z, tri0 is %b, tri1 is %b, supply1 is %b", down, up, power);
            failures = failures + 1;
        end
        drive = 1'b0;
        #1;
        $display("t=%0d tri0=%b tri1=%b supply1=%b", $time, down, up, power);
        if (down !== 1'b0 || up !== 1'b0 || power !== 1'b1) begin
            $display("FAIL: driven 0, tri0 is %b, tri1 is %b, supply1 is %b", down, up, power);
            failures = failures + 1;
        end

        if (failures != 0) begin
            $display("FAIL resolution: %0d checks failed", failures);
            $fatal(1);
        end
        $display("PASS resolution");
        $finish;
    end
endmodule
