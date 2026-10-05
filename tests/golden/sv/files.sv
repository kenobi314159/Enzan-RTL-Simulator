// SPDX-License-Identifier: GPL-3.0-or-later
//
// Run by files.sh, which compares the transcript and the files written with Icarus Verilog's.
// `$fopen` with a mode returns a file descriptor and without one a channel of a multichannel
// descriptor, whose bit 0 is the standard output (IEEE 1800 section 21.3). `$fdisplay` and
// `$fwrite` format as `$display` and `$write` do.

module tb;
    integer log_file;
    integer appended;
    integer channel_a;
    integer channel_b;
    logic [7:0] value = 8'ha5;

    initial begin
        log_file = $fopen("log.txt", "w");
        $fdisplay(log_file, "value is %h, or %0d", value, value);
        $fwrite(log_file, "no newline, ");
        $fwrite(log_file, "then one\n");
        $fdisplay(log_file, value);
        $fclose(log_file);

        // Appending, to a file opened a second time.
        appended = $fopen("appended.txt", "w");
        $fdisplay(appended, "first");
        $fclose(appended);
        appended = $fopen("appended.txt", "a");
        $fdisplay(appended, "second");
        $fclose(appended);

        // Channels combine: one write reaches each file whose bit is set, and the output.
        channel_a = $fopen("a.txt");
        channel_b = $fopen("b.txt");
        $fdisplay(channel_a | channel_b | 1, "to both channels and the output");
        $fdisplay(channel_b, "to b alone");
        $fclose(channel_a);
        $fclose(channel_b);

        $fdisplay(32'h8000_0001, "to the standard output by descriptor");

        // A file that cannot be opened gives 0.
        if ($fopen("missing/file.txt", "w") != 0) $display("a missing directory opened");
        $display("done");
        $finish;
    end
endmodule
