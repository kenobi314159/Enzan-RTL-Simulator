#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the sv-tests SystemVerilog conformance suite against Enzan.
#
#   run-suite.sh <enzan> <work-directory> [jobs]
#
# The Python used is $SVTESTS_PYTHON, or python3; `pip install -r requirements.txt` beside this
# script installs what sv-tests' driver needs.
#
# The suite is fetched at the revision in revision.txt into the work directory, and every case
# under its tests/ directory is run through sv-tests' own driver with the runner beside this
# script, so a case passes or fails by sv-tests' rules rather than by a reading of them. A case
# of a kind Enzan does not run, such as preprocessing on its own, is skipped as sv-tests skips
# it for any tool, and is in neither count.
#
# The pass rate is printed, and passing.txt is a ratchet: the run fails when a case listed there
# no longer passes, and names any case that newly does (see ../ratchet.sh).

set -uo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'usage: %s <enzan> <work-directory> [jobs]\n' "$0" >&2
    exit 1
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
mkdir -p "$2"
readonly work="$(realpath "$2")"
readonly jobs="${3:-$(nproc)}"
readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"
readonly python="${SVTESTS_PYTHON:-python3}"

if ! "$python" -c 'import psutil' 2>/dev/null; then
    printf "sv-tests' driver needs psutil, which is not installed for %s; install it with\n  %s -m pip install -r %s\n" \
        "$python" "$python" "$here/requirements.txt" >&2
    exit 1
fi

# The suite at the pinned revision, fetched once and kept.
readonly source="$work/sv-tests-$revision"
"$here/../../fetch-revision.sh" https://github.com/chipsalliance/sv-tests.git "$revision" \
    "$source" || exit 1

readonly results="$work/results"
rm -rf "$results" "$work/out"
mkdir -p "$results"

export OUT_DIR="$work/out"
export CONF_DIR="$source/conf"
export TESTS_DIR="$source/tests"
export RUNNERS_DIR="$here"
export THIRD_PARTY_DIR="$source/third_party"
export PATH="$(dirname "$enzan"):$PATH"

# One case: sv-tests' driver logs `PASS: Enzan/<case>` or `FAIL: Enzan/<case>`, or neither when
# it skips one, and that word is all that is kept. Exported so that xargs can run it in parallel.
run_case() {
    local name="$1"
    local verdict
    verdict="$("$SVTESTS_PYTHON" "$TESTS_DIR/../tools/runner" --runner Enzan --test "$name" \
        --out "$OUT_DIR/logs/$name.log" 2>&1 | sed -n 's/^.*\(PASS\|FAIL\): Enzan\/.*$/\1/p')"
    [[ -n "$verdict" ]] && printf '%s %s\n' "$verdict" "$name" >"$RESULTS/$(printf '%s' "$name" | tr '/' '%')"
    return 0
}
export -f run_case
export RESULTS="$results"
export SVTESTS_PYTHON="$python"

(cd "$TESTS_DIR" && find . -type f -iname '*.sv' | sed 's|^\./||' | sort) \
    | xargs -P "$jobs" -I{} bash -c 'run_case "$1"' _ {}

cat "$results"/* 2>/dev/null | sort -k2 >"$work/verdicts.txt"
sed -n 's/^PASS //p' "$work/verdicts.txt" >"$work/passed.txt"
sed -n 's/^FAIL //p' "$work/verdicts.txt" >"$work/failed.txt"

readonly passed_count="$(wc -l <"$work/passed.txt")"
readonly total="$(wc -l <"$work/verdicts.txt")"
if [[ "$total" -eq 0 ]]; then
    printf 'no case ran; see the logs under %s\n' "$OUT_DIR" >&2
    exit 1
fi

# A case that expects the tool to fail passes whenever Enzan fails, including on a construct it
# does not support yet. Those are counted, because that is sv-tests' definition, and said so,
# because such a pass says nothing about what Enzan accepts.
expected_failures=0
while read -r name; do
    grep -q ':should_fail_because:' "$TESTS_DIR/$name" && expected_failures=$((expected_failures + 1))
done <"$work/passed.txt"

sed 's/^/FAIL /' "$work/failed.txt"
printf '\nsv-tests %s: %d of %d cases pass (%d%%), %d of them by failing as the case expects\n' \
    "${revision:0:12}" "$passed_count" "$total" $((100 * passed_count / total)) "$expected_failures"

exec "$here/../ratchet.sh" "$here/passing.txt" "$work/passed.txt"
