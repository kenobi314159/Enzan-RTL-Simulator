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
# NEORV32's functional-simulation netlist, which netlist/make.sh makes into the directory
# XILINX_NETLISTS names, is run twice. tb_neorv32_netlist, netlist/tb_neorv32_netlist.v, is held to
# Icarus Verilog with -gspecify. neorv32_tb_netlist is NEORV32's own VHDL testbench around it,
# through the entity netlist/wrap.py writes, and is held to GHDL's run of that testbench around the
# processor's RTL, which ../neorv32/run.sh makes: the testbench's same reports, the design's own
# being no part of a netlist, and the same CPU trace and UART logs, as netlist/compare-logs.py
# compares them. That needs GHDL, GHDL or the one on the path.
#
# vhdl/*.vhd are testbenches in VHDL of the same models, each the Verilog testbench of its name
# moved across the language boundary: it reaches the Verilog models through the component
# libraries unisim and xpm, and AMD's VHDL unimacro, analysed into unimacro against unisim, and
# prints what that testbench prints. Its transcript is held to that testbench's reference, which
# is run first when this run has not. They are named vhdl/<name>.
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
readonly netlists="${XILINX_NETLISTS:-}"
readonly ghdl="${GHDL:-$(command -v ghdl)}"
source "$here/../neorv32/window.sh"
readonly unisim_components="$XILINX_VIVADO/data/vhdl/src/unisims/unisim_retarget_VCOMP.vhd"
readonly xpm_models=("$interfaces" "$models/glbl.v" "$xpm/xpm_cdc/hdl/xpm_cdc.sv"
                     "$xpm/xpm_memory/hdl/xpm_memory.sv" "$xpm/xpm_fifo/hdl/xpm_fifo.sv")

# The VHDL libraries this run has made, and the Verilog testbenches whose references it has.
declare -A made=()
declare -A referenced=()

# Makes the VHDL library <name> in the work directory, once a run, by analysing <sources> against
# the libraries made before it: false, having said why, when it cannot.
make_library() {
    local name="$1"
    shift
    [[ -n "${made[$name]:-}" ]] && return 0
    local against=()
    for library in "${!made[@]}"; do against+=(--in-lib "$work/$library"); done
    rm -rf "$work/$name"
    if ! { "$enzan" lib create --name "$name" "$work/$name" \
        && "$enzan" analyse --out-lib "$work/$name" "${against[@]}" "$@"; } \
        >"$work/$name.log" 2>&1; then
        printf 'FAIL: the library %s was not made (%s)\n' "$name" "$work/$name.log"
        return 1
    fi
    made[$name]=yes
}

readonly tested="$(grep -v '^#' "$here/release.txt" | head -1)"
if [[ "$XILINX_VIVADO" != *"$tested"* ]]; then
    printf 'WARNING: the harness last passed with Vivado %s, and XILINX_VIVADO is %s\n' "$tested" \
        "$XILINX_VIVADO"
fi

testbenches=("$@")
if [[ ${#testbenches[@]} -eq 0 ]]; then
    for file in "$here"/tb/*.sv "$here"/xpm/*.sv; do testbenches+=("$(basename "$file" .sv)"); done
    testbenches+=(tb_neorv32_netlist neorv32_tb_netlist)
    for file in "$here"/vhdl/tb_*.vhd; do testbenches+=("vhdl/$(basename "$file" .vhd)"); done
fi

# What a testbench's models read, an INIT_FILE among them, is beside it, and they name it as a file
# in the directory the simulators run in.
cp "$here"/tb/*.mem "$work"/ 2>/dev/null

status=0
skipped=0
for testbench in "${testbenches[@]}"; do
    if [[ $testbench == *neorv32*netlist ]]; then
        if [[ ! -f "$netlists/neorv32_funcsim.v" ]]; then
            printf 'NOT RUN %s: XILINX_NETLISTS names no netlists, which netlist/make.sh makes\n' \
                "$testbench"
            skipped=1
            continue
        fi
        printf '%s\n' "-y $models/unisims" "+libext+.v" "+incdir+$netlists/wrappers" \
            "$models/glbl.v" "$netlists/neorv32_funcsim.v" >"$work/$testbench.f"
    fi

    if [[ $testbench == tb_neorv32_netlist ]]; then
        source="$here/netlist/$testbench.v"
        (cd "$work" && "$enzan" --top "$testbench" --top glbl -f "$work/$testbench.f" "$source") \
            >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
        enzan_status=$?
        run_icarus "$testbench" "$source" -y "$models/unisims" -I "$netlists/wrappers" -s glbl \
            "$models/glbl.v" "$netlists/neorv32_funcsim.v"
        reference_status=$?
        judge "$testbench" "Icarus Verilog" "$enzan_status" "$reference_status" \
            "$work/$testbench.iverilog.log"
        continue
    fi

    if [[ $testbench == neorv32_tb_netlist ]]; then
        if [[ -z "$ghdl" ]]; then
            printf 'NOT RUN %s: no GHDL, which runs the RTL it is held to\n' "$testbench"
            skipped=1
            continue
        fi
        if ! "$here/../neorv32/run.sh" "$enzan" "$ghdl" "$work/neorv32" >"$work/neorv32.txt" 2>&1
        then
            printf 'FAIL %s: the RTL run it is held to failed (%s)\n' "$testbench" \
                "$work/neorv32.txt"
            status=1
            continue
        fi
        rtl="$work/neorv32/neorv32-$(grep -v '^#' "$here/../neorv32/revision.txt" | head -1)"
        mkdir -p "$work/$testbench"
        (
            cd "$work/$testbench" && rm -f "${logs[@]}"
            NEORV32_HOME="$rtl" "$enzan" --work neorv32 --top neorv32_tb --top glbl \
                "$rtl"/rtl/core/{neorv32_package,neorv32_prim,neorv32_cpu_trace}.vhd \
                "$netlists"/wrappers/{neorv32_netlist_pkg,neorv32_top}.vhd \
                "${sim_models[@]/#/$rtl/sim/}" "$rtl/sim/neorv32_tb.vhd" -f "$work/$testbench.f" \
                --ieee-asserts disable --assert-level error --run "${stop_us}us"
        ) >"$work/$testbench/output.txt" 2>"$work/$testbench/transcript.txt"
        enzan_status=$?

        # A netlist holds none of the design's own reports, which synthesis leaves out, so the
        # testbench's are what is compared.
        grep -v '^\.\./rtl/' "$work/neorv32/ghdl/transcript.txt" >"$work/$testbench/rtl-reports.txt"
        failed=
        if [[ $enzan_status -ne 0 ]]; then
            failed="Enzan exited with $enzan_status ($work/$testbench/transcript.txt)"
        elif ! "$here/../../golden/vhdl/compare-ghdl.py" \
            --enzan-transcript "$work/$testbench/transcript.txt" \
            --ghdl-transcript "$work/$testbench/rtl-reports.txt" \
            --enzan-output "$work/$testbench/output.txt" 2>"$work/$testbench/differences.txt"; then
            failed="its reports differ from the RTL's ($work/$testbench/differences.txt)"
        else
            for log in "${logs[@]}"; do
                if ! "$here/netlist/compare-logs.py" "$work/neorv32/ghdl/$log" \
                    "$work/$testbench/$log" 2>"$work/$testbench/differences.txt"; then
                    failed="$log differs from the RTL's ($work/$testbench/differences.txt)"
                    break
                fi
            done
        fi
        if [[ -n "$failed" ]]; then
            printf 'FAIL %s: %s\n' "$testbench" "$failed"
            status=1
        else
            printf 'PASS %s\n' "$testbench"
        fi
        continue
    fi

    if [[ $testbench == vhdl/* ]]; then
        name="${testbench#vhdl/}"
        stem="vhdl.$name"
        source="$here/vhdl/$name.vhd"
        if [[ -z "${referenced[$name]:-}" ]]; then
            "$here/run.sh" "$enzan" "$iverilog" "$work" "$name" >"$work/$stem.counterpart.txt"
            counterpart_status=$?
            if [[ $counterpart_status -eq 77 ]]; then
                printf 'NOT RUN %s: its Verilog counterpart did not run (%s)\n' "$testbench" \
                    "$work/$stem.counterpart.txt"
                skipped=1
                continue
            elif [[ $counterpart_status -ne 0 ]]; then
                printf 'FAIL %s: its Verilog counterpart failed (%s)\n' "$testbench" \
                    "$work/$stem.counterpart.txt"
                status=1
                continue
            fi
        fi
        if ! make_library unisim "$unisim_components"; then
            status=1
            continue
        fi
        libraries=(--in-lib "$work/unisim")
        verilog=(-f "$work/$stem.f")
        printf '%s\n' "-y $models/unisims" "+libext+.v" "$models/glbl.v" >"$work/$stem.f"
        if grep -qi '^library xpm;' "$source"; then
            make_library xpm "$xpm/xpm_VCOMP.vhd" || { status=1; continue; }
            libraries+=(--in-lib "$work/xpm")
            verilog=("${xpm_models[@]}")
        fi
        if grep -qi '^library unimacro;' "$source"; then
            macros="$XILINX_VIVADO/data/vhdl/src/unimacro"
            mapfile -t order < <(tr -d '\r' <"$macros/vhdl_analyze_order" | grep .)
            make_library unimacro "${order[@]/#/$macros/}" || { status=1; continue; }
            libraries+=(--in-lib "$work/unimacro")
        fi
        (cd "$work" && "$enzan" "${libraries[@]}" --top "$name" --top glbl \
            "$here/vhdl/verilog_format.vhd" "$source" "${verilog[@]}") \
            >"$work/$stem.enzan.txt" 2>"$work/$stem.enzan.log"
        enzan_status=$?
        cp "$work/$name.reference.txt" "$work/$stem.reference.txt"
        judge "$stem" "the Verilog testbench" "$enzan_status" 0 "$work/$stem.counterpart.txt" \
            "$name"
        continue
    fi

    if [[ -f "$here/xpm/$testbench.sv" ]]; then
        source="$here/xpm/$testbench.sv"
        if [[ -z "$verilator" ]]; then
            printf 'NOT RUN %s: no Verilator, the reference for XPM\n' "$testbench"
            skipped=1
            continue
        fi
        sources=("${xpm_models[@]}" "$source")
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
        referenced[$testbench]=yes
        continue
    fi

    source="$here/tb/$testbench.sv"
    printf '%s\n' "-y $models/unisims" "-y $models/unimacro" "+libext+.v" "$models/glbl.v" \
        "$source" >"$work/$testbench.f"
    (cd "$work" && "$enzan" --top "$testbench" --top glbl -f "$work/$testbench.f") \
        >"$work/$testbench.enzan.txt" 2>"$work/$testbench.enzan.log"
    enzan_status=$?

    run_icarus "$testbench" "$source" -y "$models/unisims" -y "$models/unimacro" -s glbl \
        "$models/glbl.v"
    reference_status=$?
    judge "$testbench" "Icarus Verilog" "$enzan_status" "$reference_status" \
        "$work/$testbench.iverilog.log"
    referenced[$testbench]=yes
done
if [[ $status -eq 0 && $skipped -ne 0 ]]; then exit 77; fi
exit "$status"
