#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercise the benchmark's result and phase recording without relying on wall-clock rates.
set -euo pipefail

bench=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat >"$work/enzan" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == --version ]]; then
    printf 'Optional components: systemverilog, vhdl\n'
    exit 0
fi
printf 'cycles=10\n-- 20 instructions, 3 delta cycles\n'
if [[ " $* " == *' --phase-timings '* && "${MOCK_TIMING:-ok}" == ok ]]; then
    printf '%s\n' '-- phase timings (ns): analysis=10 elaboration=20 bytecode=30 image=40 simulation=50' >&2
fi
EOF
chmod +x "$work/enzan"

"$bench" --phases "$work/phases.tsv" "$work/enzan" "$work/results.tsv" >"$work/output"
awk -F '\t' 'NR > 1 && (NF != 9 || $5 != 10 || $6 != 20 || $7 != 30 || $8 != 40 || $9 != 50) { exit 1 }
              END { if (NR < 2) exit 1 }' "$work/phases.tsv"
[[ $(wc -l <"$work/results.tsv") == $(wc -l <"$work/phases.tsv") ]]
awk -F '\t' 'BEGIN { OFS="\t" } NR > 1 { print $3, 20, 3, 1 }' "$work/results.tsv" >"$work/reference.tsv"
"$bench" --check "$work/reference.tsv" --phases "$work/checked-phases.tsv" \
    "$work/enzan" "$work/checked-results.tsv" >"$work/output"
[[ $(wc -l <"$work/checked-results.tsv") == $(wc -l <"$work/checked-phases.tsv") ]]

if MOCK_TIMING=missing "$bench" --phases "$work/missing.tsv" "$work/enzan" "$work/other.tsv" \
    >"$work/output" 2>&1; then
    printf 'missing timings were accepted\n' >&2
    exit 1
fi
if "$bench" --phases "$work/same.tsv" "$work/enzan" "$work/same.tsv" >"$work/output" 2>&1; then
    printf 'a shared output file was accepted\n' >&2
    exit 1
fi
printf 'PASS bench harness\n'
