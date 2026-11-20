#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run the fixed, synthetic active-cell benchmark after checking Icarus's smaller reference.
# Usage: run-scale.sh <enzan> <iverilog> <width> <work-directory>
set -euo pipefail

if [[ $# -ne 4 ]]; then
    printf 'usage: %s <enzan> <iverilog> <width> <work-directory>\n' "$0" >&2
    exit 2
fi
if [[ ! -x "$1" || ! -x "$2" ]] || ! command -v vvp >/dev/null || ! command -v python3 >/dev/null; then
    printf 'Enzan, Icarus Verilog, vvp and python3 must be executable\n' >&2
    exit 2
fi
if [[ ! "$3" =~ ^[1-9][0-9]{0,6}$ ]] || (( $3 < 32 || $3 > 1000000 || $3 % 32 != 0 )); then
    printf 'width must be a multiple of 32 between 32 and 1000000\n' >&2
    exit 2
fi
readonly enzan=$(realpath "$1")
readonly iverilog=$(realpath "$2")
readonly width=$3
readonly here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly source="$here/scale/cells.sv"
mkdir -p "$4"
readonly parent=$(realpath "$4")
readonly work=$(mktemp -d "$parent/scale.XXXXXX")
printf 'Work directory: %s\n' "$work"

# Every 32-bit seed tile follows the same recurrence. After 20 cycles its state is
# 12baddb8 (17 set bits); independently check a 1,024-cell instance with Icarus.
"$iverilog" -g2012 -s bench -Pbench.WIDTH=1024 -Pbench.TRACE=1 -o "$work/reference.vvp" "$source" \
    >"$work/icarus-build.log" 2>&1
vvp "$work/reference.vvp" >"$work/icarus.out" 2>"$work/icarus.err"
"$enzan" --top bench --timescale ns -GWIDTH=1024 -GTRACE=1 "$source" \
    >"$work/enzan-reference.out" 2>"$work/enzan-reference.err"
if [[ $(grep -c '^state=' "$work/icarus.out" || true) -ne 20 ]] || \
    ! grep -Fxq 'cells=1024 cycles=20 ones=544' "$work/icarus.out"; then
    printf 'Icarus produced an unexpected reference result; see %s\n' "$work/icarus.out" >&2
    exit 1
fi
if ! grep -Fxq 'cells=1024 cycles=20 ones=544' "$work/enzan-reference.out"; then
    printf 'Enzan produced an unexpected reference result; see %s\n' "$work/enzan-reference.out" >&2
    exit 1
fi
grep '^state=' "$work/icarus.out" >"$work/icarus.states"
grep '^state=' "$work/enzan-reference.out" >"$work/enzan-reference.states"
diff -u "$work/icarus.states" "$work/enzan-reference.states"

python3 - "$enzan" "$width" "$source" "$work" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
import time

enzan, width, source, work_arg = sys.argv[1:]
work = Path(work_arg)
command = [enzan, "--top", "bench", "--timescale", "ns", f"-GWIDTH={width}",
           "--phase-timings", source]
with (work / "enzan.out").open("w") as output, (work / "enzan.err").open("w") as errors:
    start = time.monotonic_ns()
    process = subprocess.Popen(command, stdout=output, stderr=errors)
    _, status, usage = os.wait4(process.pid, 0)
    elapsed = time.monotonic_ns() - start
    process.returncode = os.waitstatus_to_exitcode(status)
    print(f"elapsed_ns={elapsed}\npeak_rss_kib={usage.ru_maxrss}")
    if process.returncode:
        print(f"Enzan exited with status {process.returncode}; see {work / 'enzan.err'}", file=sys.stderr)
        sys.exit(1)
PY
expected="cells=$width cycles=20 ones=$((width / 32 * 17))"
if [[ $(grep -c '^cells=' "$work/enzan.out" || true) -ne 1 ]] || ! grep -Fxq "$expected" "$work/enzan.out"; then
    printf 'Enzan produced an unexpected result; see %s\n' "$work/enzan.out" >&2
    exit 1
fi
printf '%s\n' "$expected"
cat "$work/enzan.err"
printf 'PASS: reference and final result match\n'
