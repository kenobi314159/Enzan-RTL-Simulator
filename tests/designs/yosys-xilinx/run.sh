#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs testbenches of Yosys's simulation models of AMD's cells, techlibs/xilinx/cells_sim.v,
# under Enzan and under Icarus Verilog, and compares what the two print.
#
#   run.sh <enzan> <iverilog> <work-directory> [<testbench>...]
#
# They are the code cell libraries are written in, which CI can fetch: module path delays from a
# measured part and conditions that name parameters. Yosys is fetched at the revision in
# revision.txt, and its models are read in picoseconds, the unit their delays are written in. A
# testbench is judged as ../cell-testbenches.sh says, its reference Icarus Verilog running the
# module paths with -gspecify but where a testbench says Icarus cannot time its models.
#
# All the testbenches run unless some are named.

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

readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"
readonly yosys="$work/yosys-$revision"
"$here/../../fetch-revision.sh" https://github.com/YosysHQ/yosys.git "$revision" "$yosys" \
    || exit 1
readonly techlib="$yosys/techlibs/xilinx"

# The models state no time unit, and their delays are in picoseconds.
readonly models="$work/models.v"
printf '`timescale 1ps / 1ps\n`include "cells_sim.v"\n' >"$models"

testbenches=("$@")
if [[ ${#testbenches[@]} -eq 0 ]]; then
    for file in "$here"/tb/*.sv; do testbenches+=("$(basename "$file" .sv)"); done
fi

status=0
for testbench in "${testbenches[@]}"; do
    source="$here/tb/$testbench.sv"
    (cd "$work" && "$enzan" --top "$testbench" -I "$techlib" "$models" "$source") \
        >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
    enzan_status=$?

    run_icarus "$testbench" "$source" -I "$techlib" "$models"
    reference_status=$?
    judge "$testbench" "Icarus Verilog" "$enzan_status" "$reference_status" \
        "$work/$testbench.iverilog.log"
done
exit "$status"
