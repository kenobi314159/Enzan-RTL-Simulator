# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a harness holding testbenches of FPGA cell models to a reference sources: the transcripts
# compared, the run under Icarus Verilog, and the verdict on a testbench. A testbench passes when
# Enzan runs it to completion, it reports success, as `PASS <testbench>` on a line of its own, and
# Enzan's transcript is the reference's line for line.
#
# The sourcing script sets `work`, the directory every file named below is in, and `iverilog`, and
# reads `status`, which a failing testbench sets to 1.

# Each simulator announces its end, and Icarus Verilog a memory file shorter than the memory, in its
# own words, which are not the design's.
transcript() {
    grep -v -e '\$finish called at' -e '^WARNING: .*\$readmem[hb](.*): Not enough words' "$1"
}

# Runs <testbench> from <source> under Icarus Verilog, running module paths with -gspecify, the
# transcript into <work>/<testbench>.reference.txt; the rest of the arguments are iverilog's. A
# testbench whose models Icarus cannot time so says in a line of its own, `// Icarus: without
# -gspecify, since <the reason>`, and checks only values settled by then.
run_icarus() {
    local testbench="$1" source="$2"
    shift 2
    local specify=-gspecify
    if grep -q '^// Icarus: without -gspecify' "$source"; then
        specify=
        printf 'NOTE %s: %s\n' "$testbench" \
            "$(grep '^// Icarus: without -gspecify' "$source" | sed 's|^// Icarus: ||')"
    fi
    local vvp
    vvp="$(dirname "$(command -v "$iverilog")")/vvp"
    "$iverilog" -g2012 $specify -s "$testbench" -o "$work/$testbench.vvp" "$@" "$source" \
        >"$work/$testbench.iverilog.log" 2>&1 \
        && (cd "$work" && "$vvp" -n "$work/$testbench.vvp") >"$work/$testbench.reference.txt" 2>&1
}

# Judges <testbench> by the two transcripts, <work>/<testbench>.enzan.txt and .reference.txt, and
# by the exit statuses of Enzan and of <reference>, whose own log is <log>. A testbench passes as
# <passes-as>, its name unless that is given.
judge() {
    local testbench="$1" reference="$2" enzan_status="$3" reference_status="$4" log="$5"
    local passes="${6:-$1}"
    if [[ $enzan_status -ne 0 ]]; then
        printf 'FAIL %s: Enzan exited with %d (%s)\n' "$testbench" "$enzan_status" \
            "$work/$testbench.enzan.log"
        status=1
    elif [[ $reference_status -ne 0 ]]; then
        printf 'FAIL %s: %s did not run it (%s)\n' "$testbench" "$reference" "$log"
        status=1
    elif ! grep -q "^PASS $passes\$" "$work/$testbench.reference.txt"; then
        printf 'FAIL %s: the testbench does not pass under %s (%s)\n' "$testbench" "$reference" \
            "$work/$testbench.reference.txt"
        status=1
    elif ! grep -q "^PASS $passes\$" "$work/$testbench.enzan.txt"; then
        printf 'FAIL %s: the testbench did not report success (%s)\n' "$testbench" \
            "$work/$testbench.enzan.txt"
        status=1
    elif ! diff <(transcript "$work/$testbench.reference.txt") \
        <(transcript "$work/$testbench.enzan.txt") >"$work/$testbench.diff"; then
        printf 'FAIL %s: Enzan and %s disagree (%s)\n' "$testbench" "$reference" \
            "$work/$testbench.diff"
        status=1
    else
        printf 'PASS %s\n' "$testbench"
    fi
}
