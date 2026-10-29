#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs GHDL's own regression tests, its testsuite/gna, under Enzan, from GHDL fetched at the
# revision ../vests/revision.txt names, the one VESTs is run from.
#
#   run-suite.sh <enzan> <work-directory>
#
# Each test is its directory's testsuite.sh, run unchanged under GHDL's own testenv.sh with
# ghdl-shim.sh beside this script as the `ghdl` it calls, in a copy of the directory; it passes
# when the script exits 0 within a minute, as GHDL's suite_driver.sh reads it. A test of what only
# GHDL has, synthesis, VPI or VHPI, GHW waveforms, PSL, a C compiler or a feature it probes for, is
# in neither count.
#
# Prints PASS or FAIL for each test, then the counts, and writes the tests that passed to
# <work-directory>/passed.txt, for ratchet.sh to hold to passing.txt.

set -uo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <enzan> <work-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
mkdir -p "$2"
readonly work="$(realpath "$2")"
readonly revision="$(grep -v '^#' "$here/../vests/revision.txt" | head -1)"
readonly source="$work/ghdl-$revision"
"$here/../../fetch-revision.sh" https://github.com/ghdl/ghdl.git "$revision" "$source" || exit 1
readonly suite="$source/testsuite"

# Each test runs in a copy, which its script writes beside, under the testenv.sh it sources as
# ../../testenv.sh.
readonly run="$work/run"
rm -rf "$run" && mkdir -p "$run/gna"
cp "$suite/testenv.sh" "$run/testenv.sh"

tests="$work/tests.txt"
: >"$tests"
for directory in "$suite"/gna/*[0-9]*/; do
    name="$(basename "$directory")"
    [[ -f "$directory/testsuite.sh" ]] || continue
    if grep -qE 'synth|vpi|vhpi|ghw|has_feature|c_compiler|psl|PYTHON|gcc|--file-to-xml' \
        "$directory/testsuite.sh"; then
        continue
    fi
    printf '%s\n' "$name" >>"$tests"
done

run_test() {
    local name="$1"
    rm -rf "${RUN:?}/gna/$name" && cp -r "$SUITE/gna/$name" "$RUN/gna/$name"
    if (cd "$RUN/gna/$name" && timeout 60 sh ./testsuite.sh) >"$RUN/gna/$name.log" 2>&1; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s\n' "$name"
    fi
}
export -f run_test

export ENZAN="$enzan" GHDL="$here/ghdl-shim.sh" RUN="$run" SUITE="$suite"
xargs -P "$(nproc)" -I{} bash -c 'run_test "$1"' _ {} <"$tests" | sort -k2 >"$work/results.txt"
cat "$work/results.txt"

grep '^PASS ' "$work/results.txt" | cut -d' ' -f2 >"$work/passed.txt"
passed="$(wc -l <"$work/passed.txt")"
total="$(wc -l <"$work/results.txt")"
printf '\nGHDL regression: %d of %d tests pass (%d.%d%%)\n' "$passed" "$total" \
    "$((passed * 100 / total))" "$((passed * 1000 / total % 10))"

"$here/../ratchet.sh" "$here/passing.txt" "$work/passed.txt"
