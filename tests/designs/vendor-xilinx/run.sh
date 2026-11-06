#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs testbenches of AMD's own simulation models under Enzan and under Icarus Verilog, and
# compares what the two print.
#
#   run.sh <enzan> <iverilog> <work-directory> [<testbench>...]
#
# The models are read from the installation XILINX_VIVADO names, which nothing here copies, so
# without one the harness says it did not run and exits 77, as a skipped test does: a missing
# installation is not a pass. A testbench passes when Enzan runs it to completion, it reports
# success, and Enzan's transcript is Icarus Verilog's line for line, Icarus running the module paths
# with -gspecify. A testbench whose models Icarus cannot read so says it in a line of its own,
# `// Icarus: without -gspecify, since <the reason>`, and checks only values settled by then. The
# testbenches are tb/*.sv, all of them unless some are named.

set -uo pipefail

if [[ $# -lt 3 ]]; then
    printf 'usage: %s <enzan> <iverilog> <work-directory> [<testbench>...]\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
readonly iverilog="$2"
mkdir -p "$3"
readonly work="$(realpath "$3")"
shift 3
readonly vvp="$(dirname "$(command -v "$iverilog")")/vvp"

if [[ -z "${XILINX_VIVADO:-}" ]]; then
    printf 'NOT RUN: XILINX_VIVADO is not set, so there are no models to read\n'
    exit 77
fi
readonly models="$XILINX_VIVADO/data/verilog/src"
if [[ ! -d "$models/unisims" ]]; then
    printf 'NOT RUN: %s has no unisims\n' "$models"
    exit 77
fi

readonly tested="$(grep -v '^#' "$here/release.txt" | head -1)"
if [[ "$XILINX_VIVADO" != *"$tested"* ]]; then
    printf 'WARNING: the harness last passed with Vivado %s, and XILINX_VIVADO is %s\n' "$tested" \
        "$XILINX_VIVADO"
fi

# Each simulator announces its end, and Icarus Verilog a memory file shorter than the memory, in its
# own words, which are not the design's.
transcript() {
    grep -v -e '\$finish called at' -e '^WARNING: .*\$readmem[hb](.*): Not enough words' "$1"
}

testbenches=("$@")
if [[ ${#testbenches[@]} -eq 0 ]]; then
    for file in "$here"/tb/*.sv; do testbenches+=("$(basename "$file" .sv)"); done
fi

# What a testbench's models read, an INIT_FILE among them, is beside it, and they name it as a file
# in the directory the simulators run in.
cp "$here"/tb/*.mem "$work"/ 2>/dev/null

status=0
for testbench in "${testbenches[@]}"; do
    source="$here/tb/$testbench.sv"
    printf '%s\n' "-y $models/unisims" "+libext+.v" "$models/glbl.v" "$source" \
        >"$work/$testbench.f"
    (cd "$work" && "$enzan" --top "$testbench" --top glbl -f "$work/$testbench.f") \
        >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
    enzan_status=$?

    specify=-gspecify
    if grep -q '^// Icarus: without -gspecify' "$source"; then
        specify=
        printf 'NOTE %s: %s\n' "$testbench" \
            "$(grep '^// Icarus: without -gspecify' "$source" | sed 's|^// Icarus: ||')"
    fi
    "$iverilog" -g2012 $specify -y "$models/unisims" -s "$testbench" -s glbl \
        -o "$work/$testbench.vvp" "$models/glbl.v" "$source" >"$work/$testbench.iverilog.log" 2>&1 \
        && (cd "$work" && "$vvp" -n "$work/$testbench.vvp") >"$work/$testbench.iverilog.txt" 2>&1
    reference_status=$?

    if [[ $enzan_status -ne 0 ]]; then
        printf 'FAIL %s: Enzan exited with %d (%s)\n' "$testbench" "$enzan_status" \
            "$work/$testbench.enzan.log"
        status=1
    elif [[ $reference_status -ne 0 ]]; then
        printf 'FAIL %s: Icarus Verilog did not run it (%s)\n' "$testbench" \
            "$work/$testbench.iverilog.log"
        status=1
    elif ! grep -q "^PASS $testbench\$" "$work/$testbench.iverilog.txt"; then
        printf 'FAIL %s: the testbench does not pass under Icarus Verilog (%s)\n' "$testbench" \
            "$work/$testbench.iverilog.txt"
        status=1
    elif ! grep -q "^PASS $testbench\$" "$work/$testbench.enzan.txt"; then
        printf 'FAIL %s: the testbench did not report success (%s)\n' "$testbench" \
            "$work/$testbench.enzan.txt"
        status=1
    elif ! diff <(transcript "$work/$testbench.iverilog.txt") \
        <(transcript "$work/$testbench.enzan.txt") >"$work/$testbench.diff"; then
        printf 'FAIL %s: Enzan and Icarus Verilog disagree (%s)\n' "$testbench" \
            "$work/$testbench.diff"
        status=1
    else
        printf 'PASS %s\n' "$testbench"
    fi
done
exit "$status"
