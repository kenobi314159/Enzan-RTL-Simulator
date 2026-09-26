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

# Every run reads its own copy, at one path and under the source's own name. The path is part
# of the entry key, so editing a copy at a *different* path would be a miss for two reasons at
# once and would prove nothing about the text; and a script that sets a breakpoint names the
# file it was written against, which a copy called something else would not be.
mkdir -p "$work/src"
copy="$work/src/$(basename "$source_file")"
cp "$source_file" "$copy"

# The status is carried back alongside the transcript rather than aborting the script. A `do`
# file reports its own failure with `quit -code 1`, and under `set -e` a bare command
# substitution would end this script there with the transcript discarded — leaving the reason
# for the failure in the one place nobody can read it.
run() {
    "$enzan" --do "$script" "$copy" "$work/work" 2>&1
}

if ! first=$(run); then
    echo "FAIL library: the first run failed"
    echo "$first"
    exit 1
fi

if ! second=$(run); then
    echo "FAIL library: the run that should have reused the library failed"
    echo "$second"
    exit 1
fi

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

# An edit must be noticed. Only the text changes — same path, same name, same options — so a
# miss here can only be the text, which is the thing being checked.
printf '\n// an edit\n' >> "$copy"

if ! third=$(run); then
    echo "FAIL library: the run over the edited source failed"
    echo "$third"
    exit 1
fi

if grep -q 'reusing the analysed units' <<<"$third"; then
    echo "FAIL library: an edited source was served from the library"
    echo "$third"
    exit 1
fi

# The script is named, because more than one is driven through here and a bare "PASS library"
# would not say which.
echo "PASS library ($(basename "$script"))"
