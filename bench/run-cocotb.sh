#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Measure a fixed cocotb callback workload after checking it with Icarus Verilog.
# Usage: run-cocotb.sh <enzan> <python-with-cocotb> <work-directory>
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <enzan> <python-with-cocotb> <work-directory>\n' "$0" >&2
    exit 2
fi
if [[ ! -x "$1" || ! -x "$2" ]] || ! command -v iverilog >/dev/null || ! command -v vvp >/dev/null; then
    printf 'Enzan, Python with cocotb, and Icarus Verilog must be available\n' >&2
    exit 2
fi

readonly enzan=$(realpath "$1")
# Do not resolve the Python executable's symlink: its path selects the virtual environment.
readonly python="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
readonly here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly root=$(dirname "$here")
mkdir -p "$3"
readonly parent=$(realpath "$3")
readonly work=$(mktemp -d "$parent/cocotb-bench.XXXXXX")

export PATH="$(dirname "$python"):$PATH"
COCOTB_PYTHON="$python" "$root/tools/cocotb/makefiles.sh" "$work/makefiles"
for simulator in icarus enzan; do
    mkdir -p "$work/$simulator"
    cp "$here/cocotb/Makefile" "$here/cocotb/test_bench.py" \
        "$root/tests/golden/cocotb/counter.sv" "$work/$simulator/"
done

run_case() {
    local simulator=$1
    shift
    local start end
    start=$(date +%s%N)
    if ! (cd "$work/$simulator" && COCOTB_RANDOM_SEED=0 make SIM="$simulator" TOPLEVEL_LANG=verilog \
        COCOTB_MAKEFILES_DIR="$work/makefiles" COCOTB_RESULTS_FILE=results.xml "$@") \
        >"$work/$simulator.log" 2>&1; then
        cat "$work/$simulator.log" >&2
        return 1
    fi
    end=$(date +%s%N)
    case_elapsed=$((end - start))
    # A simulator that skips the test is not evidence that the workload passed.
    "$python" - "$work/$simulator/results.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
tests = root.findall(".//testcase")
if len(tests) != 1 or any(t.find(tag) is not None for t in tests
                          for tag in ("failure", "error", "skipped")):
    raise SystemExit(f"{sys.argv[1]} does not contain exactly one passing test")
PY
    if ! grep -q 'benchmark cycles=10000 count=16' "$work/$simulator.log"; then
        printf '%s did not report the expected benchmark result\n' "$simulator" >&2
        return 1
    fi
}

run_case icarus
run_case enzan ENZAN_BIN_DIR="$(dirname "$enzan")" SIM_ARGS=--phase-timings
phases=$(sed -n 's/^-- phase timings (ns): analysis=\([0-9]*\) elaboration=\([0-9]*\) bytecode=\([0-9]*\) image=\([0-9]*\) simulation=\([0-9]*\)$/\1 \2 \3 \4 \5/p' "$work/enzan.log")
if [[ ! "$phases" =~ ^[0-9]+\ [0-9]+\ [0-9]+\ [0-9]+\ [0-9]+$ ]]; then
    printf 'missing or malformed phase timings in %s\n' "$work/enzan.log" >&2
    exit 1
fi
read -r analysis elaboration bytecode image simulation <<<"$phases"
printf 'Icarus and Enzan passed the cocotb callback workload. Logs: %s\n' "$work"
printf 'cycles\telapsed_ns\tanalysis_ns\telaboration_ns\tbytecode_ns\timage_ns\tsimulation_ns\n'
printf '10000\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$case_elapsed" "$analysis" "$elaboration" "$bytecode" "$image" "$simulation"
