#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compare the fixed synchronous pipeline with Verilator after checking settled values with Icarus.
# Usage: compare-verilator.sh <enzan> <verilator> <iverilog> <work-directory>
set -euo pipefail

if [[ $# -ne 4 ]]; then
    printf 'usage: %s <enzan> <verilator> <iverilog> <work-directory>\n' "$0" >&2
    exit 2
fi
if [[ ! -x "$1" || ! -x "$2" || ! -x "$3" ]] || ! command -v vvp >/dev/null; then
    printf 'Enzan, Verilator, Icarus Verilog and vvp must be executable\n' >&2
    exit 2
fi

readonly enzan=$(realpath "$1")
readonly verilator=$(realpath "$2")
readonly iverilog=$(realpath "$3")
readonly here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly pipeline="$here/designs/pipeline.sv"
readonly monitor="$here/verilator/pipeline_monitor.sv"
mkdir -p "$4"
readonly parent=$(realpath "$4")
readonly work=$(mktemp -d "$parent/verilator-compare.XXXXXX")
printf 'Work directory: %s\n' "$work"

# Check 56 settled states after the pipeline fills, not only the final checksum. Icarus supplies
# the reference; filtering removes each simulator's own $finish notice, not design output.
"$iverilog" -g2012 -s pipeline_monitor -o "$work/monitor.vvp" "$pipeline" "$monitor" \
    >"$work/icarus-build.log" 2>&1
vvp "$work/monitor.vvp" >"$work/icarus-monitor.out" 2>"$work/icarus-monitor.err"
"$enzan" --top pipeline_monitor --timescale ns "$pipeline" "$monitor" \
    >"$work/enzan-monitor.out" 2>"$work/enzan-monitor.err"
"$verilator" --binary --timing --threads 1 -j 2 -O3 -CFLAGS -O3 -Wno-fatal \
    --top-module pipeline_monitor --Mdir "$work/monitor-obj" "$pipeline" "$monitor" \
    >"$work/verilator-monitor-build.log" 2>&1
"$work/monitor-obj/Vpipeline_monitor" >"$work/verilator-monitor.out" 2>"$work/verilator-monitor.err"
for simulator in icarus enzan verilator; do
    if ! grep -E '^(state|cycles)=' "$work/$simulator-monitor.out" >"$work/$simulator-monitor.values"; then
        printf '%s produced no validation states; see %s\n' "$simulator" "$work/$simulator-monitor.out" >&2
        exit 1
    fi
done
if [[ $(grep -c '^state=' "$work/icarus-monitor.values" || true) -ne 56 ]] \
    || ! grep -Fxq 'cycles=64 final=f3147b98' "$work/icarus-monitor.values"; then
    printf 'Icarus produced unexpected validation results; see %s\n' "$work/icarus-monitor.out" >&2
    exit 1
fi
diff -u "$work/icarus-monitor.values" "$work/enzan-monitor.values"
diff -u "$work/icarus-monitor.values" "$work/verilator-monitor.values"

# The measured case is fixed at one million cycles to make executable startup a small part of
# its run time. Icarus checks the complete run independently, before either simulator is timed.
"$iverilog" -g2012 -s bench -Pbench.CYCLES=1000000 -o "$work/benchmark.vvp" "$pipeline" \
    >"$work/icarus-benchmark-build.log" 2>&1
vvp "$work/benchmark.vvp" >"$work/icarus-benchmark.out" 2>"$work/icarus-benchmark.err"
readonly expected='cycles=1000000 final=a500fbb'
check_result() {
    local simulator=$1 file=$2
    if [[ $(grep -c '^cycles=' "$file" || true) -ne 1 ]] || ! grep -Fxq "$expected" "$file"; then
        printf '%s produced an unexpected benchmark result; see %s\n' "$simulator" "$file" >&2
        return 1
    fi
}
check_result Icarus "$work/icarus-benchmark.out"

# A fresh output directory measures translation and native compilation as a distinct cost.
build_start=$(date +%s%N)
"$verilator" --binary --timing --threads 1 -j 2 -O3 -CFLAGS -O3 -Wno-fatal \
    --top-module bench -GCYCLES=1000000 --Mdir "$work/benchmark-obj" "$pipeline" \
    >"$work/verilator-benchmark-build.log" 2>&1
build_end=$(date +%s%N)
printf 'verilator_build_ns=%s\n' "$((build_end - build_start))"
printf 'repeat\tenzan_elapsed_ns\tenzan_simulation_ns\tverilator_elapsed_ns\n'

best_enzan=0
best_verilator=0
for repeat in 1 2 3; do
    start=$(date +%s%N)
    "$enzan" --top bench --timescale ns -GCYCLES=1000000 --phase-timings "$pipeline" \
        >"$work/enzan-$repeat.out" 2>"$work/enzan-$repeat.err"
    end=$(date +%s%N)
    enzan_elapsed=$((end - start))
    check_result Enzan "$work/enzan-$repeat.out"
    simulation=$(sed -n 's/^-- phase timings (ns): analysis=[0-9]* elaboration=[0-9]* bytecode=[0-9]* image=[0-9]* simulation=\([0-9][0-9]*\)$/\1/p' "$work/enzan-$repeat.err")
    if [[ ! "$simulation" =~ ^[0-9]+$ ]]; then
        printf 'Enzan did not report phase timings; see %s\n' "$work/enzan-$repeat.err" >&2
        exit 1
    fi

    start=$(date +%s%N)
    "$work/benchmark-obj/Vbench" >"$work/verilator-$repeat.out" 2>"$work/verilator-$repeat.err"
    end=$(date +%s%N)
    verilator_elapsed=$((end - start))
    check_result Verilator "$work/verilator-$repeat.out"
    printf '%s\t%s\t%s\t%s\n' "$repeat" "$enzan_elapsed" "$simulation" "$verilator_elapsed"
    if (( best_enzan == 0 || enzan_elapsed < best_enzan )); then best_enzan=$enzan_elapsed; fi
    if (( best_verilator == 0 || verilator_elapsed < best_verilator )); then best_verilator=$verilator_elapsed; fi
done

awk -v enzan="$best_enzan" -v verilator="$best_verilator" \
    'BEGIN { printf("best_run_ratio=%.2f\n", enzan / verilator) }'
printf 'PASS: settled states and final checksums match Icarus Verilog\n'
