#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Time the pinned NEORV32 design only after its reference comparisons pass.
# Usage: run-neorv32.sh <enzan> <ghdl> <work-directory>
set -euo pipefail

if [[ $# -ne 3 ]]; then
    printf 'usage: %s <enzan> <ghdl> <work-directory>\n' "$0" >&2
    exit 2
fi
if [[ ! -x "$1" || ! -x "$2" ]]; then
    printf 'both Enzan and GHDL must be executable\n' >&2
    exit 2
fi

readonly enzan=$(realpath "$1")
readonly ghdl=$(realpath "$2")
readonly here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$3"
readonly work=$(realpath "$3")

if ! "$enzan" --version | grep -q '^Optional components:.*systemverilog.*vhdl'; then
    printf 'NEORV32 timing needs both the SystemVerilog and VHDL frontends\n' >&2
    exit 2
fi

# The existing regression owns the reference comparison, source revision and testbench window.
# The wrapper only adds the timing option; its output goes to the transcript where the harness
# ignores lines that are not HDL reports. In particular, it leaves stdout untouched because the
# VHDL comparison holds every textio line to GHDL's.
wrapper_dir=$(mktemp -d)
trap 'rm -rf "$wrapper_dir"' EXIT
cat >"$wrapper_dir/enzan" <<'EOF'
#!/usr/bin/env bash
if [[ ${1:-} == --version ]]; then
    exec "$ENZAN_BENCH_REAL" "$@"
fi
start=$(date +%s%N)
"$ENZAN_BENCH_REAL" --phase-timings "$@"
status=$?
end=$(date +%s%N)
printf '%s\n' "-- elapsed (ns): $((end - start))" >&2
exit "$status"
EOF
chmod +x "$wrapper_dir/enzan"
export ENZAN_BENCH_REAL="$enzan"

"$here/../tests/designs/neorv32/run.sh" "$wrapper_dir/enzan" "$ghdl" "$work"

printf 'design\telapsed_ns\tanalysis_ns\telaboration_ns\tbytecode_ns\timage_ns\tsimulation_ns\n'
for name in enzan enzan-verilog; do
    transcript="$work/$name/transcript.txt"
    elapsed=$(sed -n 's/^-- elapsed (ns): \([0-9][0-9]*\)$/\1/p' "$transcript")
    phases=$(sed -n 's/^-- phase timings (ns): analysis=\([0-9]*\) elaboration=\([0-9]*\) bytecode=\([0-9]*\) image=\([0-9]*\) simulation=\([0-9]*\)$/\1 \2 \3 \4 \5/p' "$transcript")
    if [[ ! "$elapsed" =~ ^[0-9]+$ || ! "$phases" =~ ^[0-9]+\ [0-9]+\ [0-9]+\ [0-9]+\ [0-9]+$ ]]; then
        printf '%s: missing or malformed timings in %s\n' "$name" "$transcript" >&2
        exit 1
    fi
    read -r analysis elaboration bytecode image simulation <<<"$phases"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$elapsed" "$analysis" "$elaboration" "$bytecode" "$image" "$simulation"
done
