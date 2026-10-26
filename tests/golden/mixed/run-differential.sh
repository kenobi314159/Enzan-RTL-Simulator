#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one design of both languages under Enzan, a VHDL testbench <name>.vhd around SystemVerilog
# modules <name>.sv, and the same design all in VHDL under GHDL, the modules as <name>_ref.vhd, and
# compares what each reported and what each signal of the testbench did (../vhdl/compare-ghdl.py
# says how). No free simulator runs both languages, so the reference is the design in one.
#
# Usage: run-differential.sh <enzan> <ghdl> <compare-ghdl.py> <directory> <name>

set -euo pipefail

if [[ $# -ne 5 ]]; then
    printf 'usage: %s <enzan> <ghdl> <compare-ghdl.py> <directory> <name>\n' "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly ghdl="$2"
readonly compare="$3"
readonly directory="$4"
readonly name="$5"

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Either simulator's run ending in a failure is part of what is compared, not a reason to stop.
(
    cd "$workdir"
    "$enzan" --top tb --vcd "$workdir/enzan.vcd" "$directory/$name.vhd" "$directory/$name.sv" \
        >"$workdir/enzan.out" 2>"$workdir/enzan.err"
) || true

(
    cd "$workdir"
    "$ghdl" -a --std=08 "$directory/${name}_ref.vhd" "$directory/$name.vhd" \
        && "$ghdl" -e --std=08 tb
) >"$workdir/ghdl-build.txt" 2>&1 || {
    printf 'GHDL could not analyse or elaborate the design:\n' >&2
    cat "$workdir/ghdl-build.txt" >&2
    exit 1
}
(
    cd "$workdir"
    "$ghdl" -r --std=08 tb --vcd=ghdl.vcd
) >"$workdir/ghdl.out" 2>&1 || true

"$compare" --scope tb --enzan-vcd "$workdir/enzan.vcd" --ghdl-vcd "$workdir/ghdl.vcd" \
    --enzan-transcript "$workdir/enzan.err" --ghdl-transcript "$workdir/ghdl.out" \
    --enzan-output "$workdir/enzan.out" || {
    printf '\nEnzan said:\n' >&2
    cat "$workdir/enzan.out" "$workdir/enzan.err" >&2
    printf '\nGHDL said:\n' >&2
    cat "$workdir/ghdl.out" >&2
    exit 1
}
