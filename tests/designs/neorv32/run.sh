#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs NEORV32 through its own testbench under Enzan and under GHDL, and compares what the two
# report and write (DESIGN_GUIDE.md section 14.5; the exit design of M6).
#
#   run.sh <enzan> <ghdl> <work-directory>
#
# The processor is fetched at the revision in revision.txt and simulated with the program image
# its repository commits, as its own sim/ghdl.sh runs it, with GHDL's --ieee-asserts=disable and
# --assert-level=error, which Enzan takes too, for 160 us: through the end of the testbench's
# on-chip debugger sequence, which halts, programs, resets and resumes the CPU over JTAG. It
# passes when both run to the end; report the same things, with the same severities, in the same
# order; and write the same CPU trace and UART logs, which hold every instruction each core
# retired with the registers and memory it touched.
#
# By an Enzan with both frontends, tb/neorv32_tb.v, the testbench in Verilog around the same VHDL
# (the first exit design of M7), is run as well, and held to the same: its displayed reports and
# the design's to GHDL's of the VHDL testbench (tb/compare-reports.py), and its logs to GHDL's.

set -uo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <enzan> <ghdl> <work-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
readonly ghdl="$(realpath "$2")"
mkdir -p "$3"
readonly work="$(realpath "$3")"
readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"
readonly compare="$here/../../golden/vhdl/compare-ghdl.py"

readonly source="$work/neorv32-$revision"
"$here/../../fetch-revision.sh" https://github.com/stnolting/neorv32.git "$revision" "$source" \
    || exit 1

readonly stop_us=160
readonly logs=(neorv32_trace0.log neorv32_trace1.log tb.uart0_rx.log tb.uart1_rx.log)

# GHDL by NEORV32's own script, which analyses with the `ghdl` it finds and runs with $GHDL.
mkdir -p "$work/ghdl"
(
    cd "$source/sim" && rm -rf build "${logs[@]}"
    PATH="$(dirname "$ghdl"):$PATH" GHDL="$ghdl" GHDL_NOLOG=1 \
        bash ./ghdl.sh --stop-time="${stop_us}us"
) >"$work/ghdl/transcript.txt" 2>&1
ghdl_status=$?
for log in "${logs[@]}"; do cp -f "$source/sim/$log" "$work/ghdl/" 2>/dev/null; done

# The script announces its own parameters, which are not the design's to compare.
sed -i '/^GHDL simulation run parameters:/d' "$work/ghdl/transcript.txt"

# Enzan on the same sources: the core's file list, then the simulation models and the testbench.
mkdir -p "$work/enzan"
(
    cd "$work/enzan" && rm -f "${logs[@]}"
    NEORV32_HOME="$source" "$enzan" --work neorv32 --top neorv32_tb \
        -f "$source/rtl/file_list_core.f" \
        "$source"/sim/{jtag_dmi_pkg,sim_uart_rx,xbus_memory,xbus_fmem,xbus_gateway}.vhd \
        "$source"/sim/{psram_model,neorv32_tb}.vhd \
        --ieee-asserts disable --assert-level error --run "${stop_us}000000000"
) >"$work/enzan/output.txt" 2>"$work/enzan/transcript.txt"
enzan_status=$?

status=0
if [[ $ghdl_status -ne 0 ]]; then
    printf 'FAIL neorv32: GHDL did not run it (%s)\n' "$work/ghdl/transcript.txt"
    status=1
fi
if [[ $enzan_status -ne 0 ]]; then
    printf 'FAIL neorv32: Enzan exited with %d (%s)\n' "$enzan_status" "$work/enzan/transcript.txt"
    status=1
fi
if [[ $status -eq 0 ]]; then
    if ! "$compare" --enzan-transcript "$work/enzan/transcript.txt" \
        --ghdl-transcript "$work/ghdl/transcript.txt" --enzan-output "$work/enzan/output.txt" \
        2>"$work/differences.txt"; then
        printf 'FAIL neorv32: the reports differ (%s)\n' "$work/differences.txt"
        status=1
    fi
    for log in "${logs[@]}"; do
        if ! cmp -s "$work/ghdl/$log" "$work/enzan/$log"; then
            printf 'FAIL neorv32: %s differs from GHDL'"'"'s\n' "$log"
            status=1
        fi
    done
fi
[[ $status -eq 0 ]] && printf 'PASS neorv32: %d reports and %d logs as GHDL has them\n' \
    "$(grep -c '(report\|(assertion' "$work/ghdl/transcript.txt")" "${#logs[@]}"

# The Verilog testbench, on the same sources but the VHDL testbench and its JTAG package.
if [[ $ghdl_status -eq 0 ]] \
    && "$enzan" --version | grep -q '^Optional components:.*systemverilog.*vhdl'; then
    mkdir -p "$work/enzan-verilog"
    (
        cd "$work/enzan-verilog" && rm -f "${logs[@]}"
        NEORV32_HOME="$source" "$enzan" --work neorv32 --top neorv32_tb \
            -f "$source/rtl/file_list_core.f" \
            "$source"/sim/{sim_uart_rx,xbus_memory,xbus_fmem,xbus_gateway,psram_model}.vhd \
            "$here/tb/neorv32_tb.v" \
            --ieee-asserts disable --assert-level error --run "${stop_us}000000000"
    ) >"$work/enzan-verilog/output.txt" 2>"$work/enzan-verilog/transcript.txt"
    verilog_status=$?
    if [[ $verilog_status -ne 0 ]]; then
        printf 'FAIL neorv32 (Verilog testbench): Enzan exited with %d (%s)\n' "$verilog_status" \
            "$work/enzan-verilog/transcript.txt"
        status=1
    else
        verilog_passed=yes
        if ! "$here/tb/compare-reports.py" --ghdl-transcript "$work/ghdl/transcript.txt" \
            --enzan-transcript "$work/enzan-verilog/transcript.txt" \
            --enzan-output "$work/enzan-verilog/output.txt" \
            2>"$work/enzan-verilog/differences.txt"; then
            printf 'FAIL neorv32 (Verilog testbench): the reports differ (%s)\n' \
                "$work/enzan-verilog/differences.txt"
            verilog_passed=no
        fi
        for log in "${logs[@]}"; do
            if ! cmp -s "$work/ghdl/$log" "$work/enzan-verilog/$log"; then
                printf 'FAIL neorv32 (Verilog testbench): %s differs from GHDL'"'"'s\n' "$log"
                verilog_passed=no
            fi
        done
        if [[ $verilog_passed == yes ]]; then
            printf 'PASS neorv32 (Verilog testbench): its reports and %d logs as GHDL has them\n' \
                "${#logs[@]}"
        else
            status=1
        fi
    fi
fi
exit "$status"
