// SPDX-License-Identifier: GPL-3.0-or-later
//
// AMD's XPM simple dual-port block RAM, 32 bits by 1024, common clock, two cycles of read latency:
// words written through port A are read back through port B, the latency counted, and a word
// rewritten reads as its new value. Nothing depends on x, which a two-state reference cannot show.

`timescale 1ns / 1ps

module tb_xpm_memory;

    logic clk = 1'b0;
    always #5 clk = ~clk;

    logic [9:0] addra = '0, addrb = '0;
    logic [31:0] dina = '0;
    logic wea = 1'b0, rstb = 1'b1;
    wire [31:0] doutb;

    xpm_memory_sdpram #(.MEMORY_SIZE(32768), .MEMORY_PRIMITIVE("block"),
                        .CLOCKING_MODE("common_clock"), .WRITE_DATA_WIDTH_A(32),
                        .READ_DATA_WIDTH_B(32), .BYTE_WRITE_WIDTH_A(32), .ADDR_WIDTH_A(10),
                        .ADDR_WIDTH_B(10), .READ_LATENCY_B(2), .WRITE_MODE_B("read_first"))
        u_ram (.clka(clk), .clkb(clk), .ena(1'b1), .enb(1'b1), .wea(wea), .addra(addra),
               .addrb(addrb), .dina(dina), .doutb(doutb), .rstb(rstb), .regceb(1'b1),
               .sleep(1'b0), .injectsbiterra(1'b0), .injectdbiterra(1'b0), .sbiterrb(),
               .dbiterrb());

    int failures = 0;
    initial begin
        #200;
        @(negedge clk) rstb = 1'b0;
        for (int address = 0; address < 16; address++) begin
            @(negedge clk);
            addra = 10'(address);
            dina = 32'h1000_0001 * (address + 3);
            wea = 1'b1;
        end
        @(negedge clk) wea = 1'b0;

        for (int address = 0; address < 16; address++) begin
            addrb = 10'(address);
            repeat (2) @(negedge clk);
            $display("read %0d: %h", address, doutb);
            if (doutb != 32'h1000_0001 * (address + 3)) failures = failures + 1;
        end

        // A rewrite, read back.
        addra = 10'd5;
        dina = 32'hcafe_f00d;
        wea = 1'b1;
        @(negedge clk) wea = 1'b0;
        addrb = 10'd5;
        repeat (2) @(negedge clk);
        $display("reread 5: %h", doutb);
        if (doutb != 32'hcafe_f00d) failures = failures + 1;

        if (failures == 0) $display("PASS tb_xpm_memory");
        $finish;
    end
endmodule
