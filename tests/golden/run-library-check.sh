#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M3's other exit criterion: libraries reused across runs without re-analysis
# (DESIGN_GUIDE.md section 18).
#
# Reuse is a property of two processes, not of one, so it cannot be checked from inside a
# single `do` script. This runs the same script twice against one library directory and
# requires the first run to analyse, the second to reuse, and both to reach the same answer —
# the last of which is the part that matters, because a library that is fast and wrong is far
# worse than one that is slow.

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <enzan> <script.do> <source.sv>" >&2
    exit 2
fi

enzan=$1
script=$2
source_file=$3

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run() {
    "$enzan" --do "$script" "$source_file" "$work/work" 2>&1
}

first=$(run)
second=$(run)

if grep -q 'reusing the analysed units' <<<"$first"; then
    echo "FAIL library: the first run reused a library that did not exist yet"
    echo "$first"
    exit 1
fi

if ! grep -q 'reusing the analysed units' <<<"$second"; then
    echo "FAIL library: the second run analysed from source again"
    echo "$second"
    exit 1
fi

# The transcript either side of the "reusing" line must be identical: same hierarchy, same
# values, same finishing time. A library that changed the answer would show up here.
strip() { grep -v 'reusing the analysed units' <<<"$1"; }

if ! diff <(strip "$first") <(strip "$second") >/dev/null; then
    echo "FAIL library: the reused run did not produce the same transcript"
    diff <(strip "$first") <(strip "$second") || true
    exit 1
fi

# An edit must be noticed. The text changes, so the key changes, so the entry is a miss.
edited="$work/edited.sv"
{ cat "$source_file"; echo; echo "// an edit"; } > "$edited"

third=$("$enzan" --do "$script" "$edited" "$work/work" 2>&1)
if grep -q 'reusing the analysed units' <<<"$third"; then
    echo "FAIL library: an edited source was served from the library"
    echo "$third"
    exit 1
fi

echo "PASS library"
