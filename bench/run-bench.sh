#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Measures simulated clock cycles per second and appends a row to the results file.
#
# What is binding is not a performance number but that cycles/sec are recorded per commit, from
# the first simulation onwards. A performance goal without continuous measurement is
# decoration, and a regression is invisible without a baseline.
#
# Usage: run-bench.sh <enzan-binary> [results-file]
#
# The results file is tab separated, one row per run:
#
#   timestamp  revision  design  cycles  seconds  cycles_per_second  instructions  deltas
#
# Tab separated rather than JSON so that it can be appended to, sorted, and plotted with the
# tools already on any machine, and so that a diff of two runs is readable.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    printf 'usage: %s <enzan-binary> [results-file]\n' "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly results="${2:-$here/results.tsv}"

if [[ ! -x "$enzan" ]]; then
    printf '%s is not executable\n' "$enzan" >&2
    exit 1
fi

revision="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if ! git -C "$here" diff --quiet HEAD 2>/dev/null; then
    # A measurement from a dirty tree is not attributable to a commit, and silently recording
    # it as though it were is how a performance history becomes untrustworthy.
    revision="${revision}-dirty"
fi

if [[ ! -f "$results" ]]; then
    printf 'timestamp\trevision\tdesign\tcycles\tseconds\tcycles_per_second\tinstructions\tdeltas\n' \
        >"$results"
fi

printf '%-16s %12s %12s %14s\n' design cycles seconds cycles/sec

# A design is in the language its extension names, and one the build has no frontend for is
# skipped, as a build of one language measures that language's designs.
components="$("$enzan" --version 2>/dev/null | sed -n 's/^Optional components: //p' | tr ',' ' ')"

status=0
for design in "$here"/designs/*.sv "$here"/designs/*.vhd; do
    [[ -f "$design" ]] || continue
    case "$design" in
    *.sv) language=systemverilog name="$(basename "$design" .sv)" ;;
    *.vhd) language=vhdl name="$(basename "$design" .vhd)-vhdl" ;;
    esac
    if [[ " $components " != *" $language "* ]]; then
        printf '%-16s skipped: no %s frontend in this build\n' "$name" "$language"
        continue
    fi
    output="$(mktemp)"

    # The elapsed time covers analysis, elaboration and the run, which is what a user waits
    # for. Separating them is worth doing once there is something to tune; conflating them now
    # at least never flatters the result.
    start="$(date +%s.%N)"
    if ! "$enzan" --top bench --timescale ns --dump-stats "$design" >"$output" 2>&1; then
        printf '%-16s FAILED\n' "$name"
        cat "$output" >&2
        rm -f "$output"
        status=1
        continue
    fi
    finish="$(date +%s.%N)"

    # SystemVerilog's $display prints the count at the start of its line, and VHDL's report
    # after the location it reports.
    cycles="$(sed -n 's/^\(.*: note: \)\{0,1\}cycles=\([0-9][0-9]*\).*/\2/p' "$output" | head -1)"
    instructions="$(sed -n 's/^-- \([0-9]*\) instructions.*/\1/p' "$output" | head -1)"
    deltas="$(sed -n 's/^-- .* \([0-9]*\) delta cycles.*/\1/p' "$output" | head -1)"
    rm -f "$output"

    if [[ -z "$cycles" || "$cycles" == 0 ]]; then
        printf '%-16s reported no cycle count\n' "$name" >&2
        status=1
        continue
    fi

    # The printf arguments are parenthesised: awk reads an unparenthesised `printf x, y > 0`
    # as a redirection to the file named "0" rather than as a comparison.
    seconds="$(awk -v a="$start" -v b="$finish" 'BEGIN { printf("%.3f", b - a) }')"
    rate="$(awk -v c="$cycles" -v s="$seconds" 'BEGIN { printf("%.0f", s > 0 ? c / s : 0) }')"

    printf '%-16s %12s %12s %14s\n' "$name" "$cycles" "$seconds" "$rate"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -Iseconds)" "$revision" "$name" "$cycles" \
        "$seconds" "$rate" "${instructions:-0}" "${deltas:-0}" >>"$results"
done

printf '\nAppended to %s\n' "$results"
exit "$status"
