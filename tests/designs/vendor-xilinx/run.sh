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
# installation is not a pass. A testbench is judged as ../cell-testbenches.sh says, its reference
# Icarus Verilog running the module paths with -gspecify, but where a testbench says Icarus cannot
# time its models. The testbenches are tb/*.sv, all of them unless some are named.

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
source "$here/../cell-testbenches.sh"

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

    run_icarus "$testbench" "$source" -y "$models/unisims" -s glbl "$models/glbl.v"
    reference_status=$?
    judge "$testbench" "Icarus Verilog" "$enzan_status" "$reference_status" \
        "$work/$testbench.iverilog.log"
done
exit "$status"
