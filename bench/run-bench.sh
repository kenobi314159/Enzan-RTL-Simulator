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
# The results file is tab separated, one row per design and run:
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

# Each design runs this many times and the fastest is recorded: what slows a run down besides
# Enzan, another process or a cold cache, only ever slows it down.
readonly repeats=3

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

# Whether `$1` is more than `$2` by over `$3` percent of `$2`.
over() { awk -v a="$1" -v b="$2" -v p="$3" 'BEGIN { exit !(a > b * (1 + p / 100)) }'; }

status=0
for design in "$here"/designs/*.sv "$here"/designs/*.vhd "$here"/designs/*.f; do
    [[ -f "$design" ]] || continue

    # A file list names a design of several sources, which is how a benchmark reads a golden
    # case's sources where they are. Its languages are those of the sources it lists.
    case "$design" in
    *.sv) languages=systemverilog name="$(basename "$design" .sv)" sources=("$design") ;;
    *.vhd) languages=vhdl name="$(basename "$design" .vhd)-vhdl" sources=("$design") ;;
    *.f)
        name="$(basename "$design" .f)"
        sources=(-F "$design")
        languages="$(sed -e 's|//.*||' -e 's/[[:space:]]*$//' "$design" |
            sed -n -e 's/\.sv$/ systemverilog/p' -e 's/\.vhd$/ vhdl/p' |
            awk '{ print $NF }' | sort -u | tr '\n' ' ')"
        ;;
    esac
    missing=""
    for language in $languages; do
        [[ " $components " == *" $language "* ]] || missing="$language"
    done
    if [[ -n "$missing" ]]; then
        printf '%-16s skipped: no %s frontend in this build\n' "$name" "$missing"
        continue
    fi
    output="$(mktemp)"

    best=""
    failed=0
    instructions=""
    deltas=""
    for ((run = 0; run < repeats; run++)); do
        # The elapsed time covers analysis, elaboration and the run, which is what a user
        # waits for. Separating them is worth doing once there is something to tune;
        # conflating them now at least never flatters the result.
        start="$(date +%s.%N)"
        if ! "$enzan" --top bench --timescale ns --dump-stats "${sources[@]}" >"$output" 2>&1; then
            failed=1
            break
        fi
        finish="$(date +%s.%N)"

        # The printf arguments are parenthesised: awk reads an unparenthesised `printf x, y > 0`
        # as a redirection to the file named "0" rather than as a comparison.
        seconds="$(awk -v a="$start" -v b="$finish" 'BEGIN { printf("%.3f", b - a) }')"
        if [[ -z "$best" ]] || over "$best" "$seconds" 0; then
            best="$seconds"
        fi

        # SystemVerilog's $display prints the count at the start of its line, and VHDL's report
        # after the location it reports.
        cycles="$(sed -n 's/^\(.*: note: \)\{0,1\}cycles=\([0-9][0-9]*\).*/\2/p' "$output" | head -1)"
        counts="$(sed -n 's/^-- \([0-9]*\) instructions, \([0-9]*\) delta cycles.*/\1 \2/p' \
            "$output" | head -1)"

        # The counts are the same on every run of one build, or the simulation is not
        # deterministic, and a reference for them would mean nothing.
        if [[ -n "$instructions" && "$counts" != "$instructions $deltas" ]]; then
            printf '%-16s FAILED: counted %s, then %s\n' "$name" "$instructions $deltas" \
                "$counts" >&2
            failed=2
            break
        fi
        read -r instructions deltas <<<"$counts"
    done

    if [[ "$failed" == 1 ]]; then
        printf '%-16s FAILED\n' "$name"
        cat "$output" >&2
    fi
    rm -f "$output"
    if [[ "$failed" != 0 ]]; then
        status=1
        continue
    fi

    if [[ -z "$cycles" || "$cycles" == 0 ]]; then
        printf '%-16s reported no cycle count\n' "$name" >&2
        status=1
        continue
    fi

    rate="$(awk -v c="$cycles" -v s="$best" 'BEGIN { printf("%.0f", s > 0 ? c / s : 0) }')"

    printf '%-16s %12s %12s %14s\n' "$name" "$cycles" "$best" "$rate"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -Iseconds)" "$revision" "$name" "$cycles" \
        "$best" "$rate" "${instructions:-0}" "${deltas:-0}" >>"$results"
done

printf '\nAppended to %s\n' "$results"
exit "$status"
