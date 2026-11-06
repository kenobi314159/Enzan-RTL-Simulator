#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs testbenches of AMD's own simulation models under Enzan and under a reference, and compares
# what the two print.
#
#   run.sh <enzan> <iverilog> <work-directory> [<testbench>...]
#
# The models are read from the installation XILINX_VIVADO names, which nothing here copies, so
# without one the harness says it did not run and exits 77, as a skipped test does: a missing
# installation is not a pass, and neither is a missing reference. A testbench is judged as
# ../cell-testbenches.sh says.
#
# tb/*.sv are the unisims' testbenches, whose reference is Icarus Verilog running the module paths
# with -gspecify, but where a testbench says Icarus cannot time its models.
#
# xpm/*.sv are XPM's, whose reference is Verilator, VERILATOR or the one on the path, since Icarus
# reads none of XPM; they check nothing a two-state simulator cannot show. Verilator is given
# ONESPIN, which leaves out what xpm_cdc forces while glbl's reset is high, since Verilator cannot
# force a whole unpacked array; every testbench starts after that reset, and resets what it uses.
# Verilator takes one top level, so it is given one holding the testbench and glbl.
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

if [[ -z "${XILINX_VIVADO:-}" ]]; then
    printf 'NOT RUN: XILINX_VIVADO is not set, so there are no models to read\n'
    exit 77
fi
readonly models="$XILINX_VIVADO/data/verilog/src"
if [[ ! -d "$models/unisims" ]]; then
    printf 'NOT RUN: %s has no unisims\n' "$models"
    exit 77
fi

readonly xpm="$XILINX_VIVADO/data/ip/xpm"
readonly interfaces="$XILINX_VIVADO/../data/rsb/busdef/vivado_interfaces.svh"
readonly verilator="${VERILATOR:-$(command -v verilator)}"

readonly tested="$(grep -v '^#' "$here/release.txt" | head -1)"
if [[ "$XILINX_VIVADO" != *"$tested"* ]]; then
    printf 'WARNING: the harness last passed with Vivado %s, and XILINX_VIVADO is %s\n' "$tested" \
        "$XILINX_VIVADO"
fi

testbenches=("$@")
if [[ ${#testbenches[@]} -eq 0 ]]; then
    for file in "$here"/tb/*.sv "$here"/xpm/*.sv; do testbenches+=("$(basename "$file" .sv)"); done
fi

# What a testbench's models read, an INIT_FILE among them, is beside it, and they name it as a file
# in the directory the simulators run in.
cp "$here"/tb/*.mem "$work"/ 2>/dev/null

status=0
skipped=0
for testbench in "${testbenches[@]}"; do
    if [[ -f "$here/xpm/$testbench.sv" ]]; then
        source="$here/xpm/$testbench.sv"
        if [[ -z "$verilator" ]]; then
            printf 'NOT RUN %s: no Verilator, the reference for XPM\n' "$testbench"
            skipped=1
            continue
        fi
        sources=("$interfaces" "$models/glbl.v" "$xpm/xpm_cdc/hdl/xpm_cdc.sv"
                 "$xpm/xpm_memory/hdl/xpm_memory.sv" "$xpm/xpm_fifo/hdl/xpm_fifo.sv" "$source")
        (cd "$work" && "$enzan" --top "$testbench" --top glbl "${sources[@]}") \
            >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
        enzan_status=$?

        mkdir -p "$work/$testbench.verilator"
        printf 'module verilator_top;\n    %s tb ();\n    glbl glbl ();\nendmodule\n' "$testbench" \
            >"$work/$testbench.verilator/top.sv"
        "$verilator" --binary --timing -Wno-fatal -Wno-lint -Wno-style -Wno-MULTIDRIVEN \
            -Wno-SYMRSVDWORD +define+ONESPIN --top-module verilator_top \
            -Mdir "$work/$testbench.verilator" "${sources[@]}" "$work/$testbench.verilator/top.sv" \
            >"$work/$testbench.verilator.log" 2>&1 \
            && (cd "$work" && "$work/$testbench.verilator/Vverilator_top") 2>&1 \
            | grep -v -E '^- |^\[[0-9]+\] -(Info|Warning)|^ *$' >"$work/$testbench.reference.txt"
        reference_status=${PIPESTATUS[0]}
        judge "$testbench" Verilator "$enzan_status" "$reference_status" \
            "$work/$testbench.verilator.log"
        continue
    fi

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
if [[ $status -eq 0 && $skipped -ne 0 ]]; then exit 77; fi
exit "$status"
