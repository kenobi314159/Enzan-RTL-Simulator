#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one VHDL design under Enzan and under GHDL and compares what each reported and what every
# signal did (compare-ghdl.py says how).
#
# Usage: run-ghdl-differential.sh <enzan> <ghdl> <compare-ghdl.py> <source.vhd> <top>

set -euo pipefail

if [[ $# -ne 5 ]]; then
    printf 'usage: %s <enzan> <ghdl> <compare-ghdl.py> <source.vhd> <top>\n' "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly ghdl="$2"
readonly compare="$3"
readonly source_file="$4"
readonly top="$5"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Either simulator's run ending in a failure is part of what is compared, not a reason to stop.
"$enzan" --top "$top" --vcd "$workdir/enzan.vcd" "$source_file" >"$workdir/enzan.out" \
    2>"$workdir/enzan.err" || true

(
    cd "$workdir"
    "$ghdl" -a --std=08 "$source_file" && "$ghdl" -e --std=08 "$top"
) >"$workdir/ghdl-build.txt" 2>&1 || {
    printf 'GHDL could not analyse or elaborate the design:\n' >&2
    cat "$workdir/ghdl-build.txt" >&2
    exit 1
}
(
    cd "$workdir"
    "$ghdl" -r --std=08 "$top" --vcd=ghdl.vcd
) >"$workdir/ghdl.out" 2>&1 || true

"$compare" --enzan-vcd "$workdir/enzan.vcd" --ghdl-vcd "$workdir/ghdl.vcd" \
    --enzan-transcript "$workdir/enzan.err" --ghdl-transcript "$workdir/ghdl.out" || {
    printf '\nEnzan said:\n' >&2
    cat "$workdir/enzan.err" >&2
    printf '\nGHDL said:\n' >&2
    cat "$workdir/ghdl.out" >&2
    exit 1
}
