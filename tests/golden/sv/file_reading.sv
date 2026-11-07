// SPDX-License-Identifier: GPL-3.0-or-later
//
// Run by run-isolated.sh, which compares the transcript and the files written with Icarus
// Verilog's. A file read a byte at a time by `$fgetc`, which gives -1 at the end (IEEE 1800
// section 21.3.4.1), and opened by `$fopen` with its name and mode held in vectors, as Verilog
// holds a string (section 21.3.1). Altera's models read a memory file so: they convert it, a line
// at a time, into one that `$readmemh` then loads.

module tb;
    reg [8*16:1] name, converted;
    reg [8*2:1] mode;
    integer out, in, c, count, value, words, failures;
    reg done, comment;
    reg [7:0] memory [0:3];

    initial begin
        failures = 0;
        name = "words.mif";
        mode = "w";
        out = $fopen(name, mode);
        $fwrite(out, "-- a comment\n1F\n2a\n03\n40\n");
        $fclose(out);

        // Each line's hex digits make a word, written out one a line; a comment line is skipped.
        // An iteration ends early by `disable` of the loop's body, as the models' readers end
        // theirs.
        converted = "words.ver";
        in = $fopen(name, "r");
        out = $fopen(converted, "w");
        count = 0;
        value = 0;
        words = 0;
        done = 0;
        comment = 0;
        while (!done) begin : reader
            c = $fgetc(in);
            count = count + 1;
            if (c == -1) begin
                done = 1;
                disable reader;
            end
            if (c == "-") comment = 1;
            if (c == "\n") begin
                if (!comment) begin
                    $fdisplay(out, "%h", value[7:0]);
                    words = words + 1;
                end
                comment = 0;
                value = 0;
                disable reader;
            end
            if (comment) disable reader;
            if (c >= "0" && c <= "9") value = value * 16 + c - "0";
            else if (c >= "a" && c <= "f") value = value * 16 + c - "a" + 10;
            else if (c >= "A" && c <= "F") value = value * 16 + c - "A" + 10;
        end
        $fclose(in);
        $fclose(out);

        $readmemh(converted, memory);
        $display("%0d reads, %0d words: %h %h %h %h", count, words, memory[0], memory[1],
                 memory[2], memory[3]);
        if (count != 26 || words != 4) failures = failures + 1;
        if (memory[0] != 8'h1f || memory[1] != 8'h2a || memory[2] != 8'h03 || memory[3] != 8'h40)
            failures = failures + 1;

        // The twelve bytes written, and then the end.
        in = $fopen(converted, "r");
        repeat (13) c = $fgetc(in);
        $display("past the end: %0d", c);
        if (c != -1) failures = failures + 1;
        $fclose(in);

        if (failures == 0) $display("PASS file_reading");
        else $display("FAIL: file_reading");
        $finish;
    end
endmodule
