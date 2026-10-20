#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one design of both languages under Enzan, a SystemVerilog testbench <name>.sv around VHDL
# units <name>.vhd, and the same design all in SystemVerilog under Icarus Verilog, the units as
# <name>_ref.sv, and compares what each printed: the testbench's transcript, at settled times, as
# ../run-differential.sh compares a design of SystemVerilog alone (DESIGN_GUIDE.md section 14.3).
#
# Usage: run-sv-differential.sh <enzan> <iverilog> <directory> <name>

set -euo pipefail

if [[ $# -ne 4 ]]; then
    printf 'usage: %s <enzan> <iverilog> <directory> <name>\n' "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly reference="$2"
readonly directory="$3"
readonly name="$4"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

"$enzan" --top tb "$directory/$name.sv" "$directory/$name.vhd" >"$workdir/enzan.txt" \
    2>"$workdir/enzan-diagnostics.txt" || {
    printf 'Enzan failed:\n' >&2
    cat "$workdir/enzan.txt" "$workdir/enzan-diagnostics.txt" >&2
    exit 1
}
"$reference" -g2012 -o "$workdir/reference.vvp" "$directory/$name.sv" "$directory/${name}_ref.sv" \
    >"$workdir/compile.txt" 2>&1 || {
    printf 'the reference simulator failed to compile the design:\n' >&2
    cat "$workdir/compile.txt" >&2
    exit 1
}
"$workdir/reference.vvp" >"$workdir/reference.txt" 2>"$workdir/reference-stderr.txt" || {
    printf 'the reference simulator failed:\n' >&2
    cat "$workdir/reference.txt" >&2
    exit 1
}

# Each simulator announces its own exit in its own words, which is not the design's behaviour.
normalise() {
    grep -vE '\$finish called at|\$stop called at|^\*\* ' "$1" | sed -e 's/[[:space:]]*$//'
}
if ! diff -u <(normalise "$workdir/reference.txt") <(normalise "$workdir/enzan.txt"); then
    printf '\nEnzan and %s disagree about %s.\n' "$reference" "$name" >&2
    printf 'The left side is the reference, the right side is Enzan.\n' >&2
    exit 1
fi
