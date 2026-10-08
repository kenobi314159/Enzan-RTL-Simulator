#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs secworks' AES core through its own testbenches under Enzan and under Icarus Verilog, and
# compares what the two print (DESIGN_GUIDE.md section 14.5; the first exit design of M5).
#
#   run.sh <enzan> <iverilog> <work-directory>
#
# The design is fetched at the revision in revision.txt. A testbench passes when Enzan runs it to
# completion, it reports every test case successful, and Enzan's transcript is Icarus Verilog's
# line for line. The testbenches check themselves against the NIST vectors, so a match is two
# independent simulators agreeing on a design that also checks its own answers.
#
# tb_aes and tb_aes_core are run. The other three are not valid SystemVerilog at this revision —
# two dump a scope that does not exist and one reads signals the RTL no longer has — and slang
# refuses them where Icarus Verilog lets them through.

set -uo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <enzan> <iverilog> <work-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
readonly iverilog="$2"
mkdir -p "$3"
readonly work="$(realpath "$3")"
readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"
readonly vvp="$(dirname "$(command -v "$iverilog")")/vvp"

readonly source="$work/aes-$revision"
"$here/../../fetch-revision.sh" https://github.com/secworks/aes.git "$revision" "$source" || exit 1

# Each simulator announces itself and its end in its own words, which are not the design's.
transcript() {
    grep -v -e '^VCD info:' -e '\$finish called at' "$1"
}

status=0
for testbench in tb_aes tb_aes_core; do
    sources=("$source/src/tb/$testbench.v" "$source"/src/rtl/*.v)
    (cd "$work" && "$enzan" --top "$testbench" --timescale ns "${sources[@]}") \
        >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
    enzan_status=$?

    "$iverilog" -g2012 -s "$testbench" -o "$work/$testbench.vvp" "${sources[@]}" \
        >"$work/$testbench.iverilog.log" 2>&1 \
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
    elif ! grep -q 'test cases completed successfully' "$work/$testbench.enzan.txt"; then
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
