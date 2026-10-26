#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one design under Enzan and under a reference simulator and compares their transcripts.
#
# Differential testing is "the single highest-value testing strategy available" and must exist
# from M1 (DESIGN_GUIDE.md section 14.3). Comparing *transcripts* rather than waveforms is
# deliberate: section 14.3 warns that two correct simulators may legitimately differ in
# delta-level ordering, so a comparison has to be made at settled time boundaries. A
# self-checking testbench's `$display` output is exactly that — it happens after a delay, when
# the design has settled — whereas a VCD diff compares every delta and produces constant false
# failures until somebody switches the suite off.
#
# Usage: run-differential.sh <enzan> <reference> <source.sv> <top> [<enzan option>...]
#
# The options are Enzan's time unit, `--timescale ns`, or none for a design whose every module
# states its own time scale.

set -euo pipefail

if [[ $# -lt 4 ]]; then
    printf 'usage: %s <enzan> <reference-simulator> <source.sv> <top> [<enzan option>...]\n' \
        "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly reference="$2"
readonly source_file="$3"
readonly top="$4"
shift 4

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Only the *design's* output is compared, so stdout and stderr are kept apart.
#
# Enzan writes `$display` to stdout and diagnostics to stderr, which is the separation a
# scriptable tool should have. Merging them would make this test compare Enzan's warnings
# against Icarus's, and the two tools warn about different things by design — Enzan inherits
# slang's diagnostics, which are deliberately better than most (DESIGN_GUIDE.md section 10.6).
#
# `$time` counts each module's own unit in both, so a transcript compares whatever the run's is.
"$enzan" --top "$top" "$@" "$source_file" >"$workdir/enzan.txt" \
    2>"$workdir/enzan-diagnostics.txt" || {
    printf 'Enzan failed:\n' >&2
    cat "$workdir/enzan.txt" "$workdir/enzan-diagnostics.txt" >&2
    exit 1
}

"$reference" -g2012 -o "$workdir/reference.vvp" "$source_file" >"$workdir/compile.txt" 2>&1 || {
    printf 'the reference simulator failed to compile the design:\n' >&2
    cat "$workdir/compile.txt" >&2
    exit 1
}

"$workdir/reference.vvp" >"$workdir/reference.txt" 2>"$workdir/reference-stderr.txt" || {
    printf 'the reference simulator failed:\n' >&2
    cat "$workdir/reference.txt" >&2
    exit 1
}

# Each simulator announces its own exit in its own words, which is not part of the design's
# behaviour. Nothing else is filtered: a difference in what the design printed is exactly what
# this test exists to catch.
normalise() {
    # Icarus prefixes its exit notice with the source location, so the pattern is not
    # anchored. Nothing else is filtered: a difference in what the *design* printed is exactly
    # what this test exists to catch.
    grep -vE '\$finish called at|\$stop called at|^\*\* ' "$1" | sed -e 's/[[:space:]]*$//'
}

if ! diff -u <(normalise "$workdir/reference.txt") <(normalise "$workdir/enzan.txt"); then
    printf '\nEnzan and %s disagree about %s.\n' "$reference" "$source_file" >&2
    printf 'The left side is the reference, the right side is Enzan.\n' >&2
    exit 1
fi

printf 'Enzan and %s agree about %s.\n' "$(basename "$reference")" "$(basename "$source_file")"
