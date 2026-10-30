#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the AXI-stream components of fpganinja's taxi through their own cocotb testbenches under
# Enzan.
#
#   run.sh <enzan> <work-directory>
#
# The cocotb used is the one installed for $COCOTB_PYTHON, or for python3, with what
# requirements.txt beside this script lists. taxi is fetched at the revision in revision.txt,
# and each testbench under src/axis/tb runs with SIM=enzan as taxi's own makefile runs it. The
# parameters its makefile sets are passed with -G, since the makefile does that only for the
# simulators it knows. A case passes by cocotb's own verdict; expected.txt names the testbenches
# and cases that are not expected to, and why, and any other failure fails the run.

set -uo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <enzan> <work-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
mkdir -p "$2"
readonly work="$(realpath "$2")"
readonly python="${COCOTB_PYTHON:-python3}"
readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"

if ! "$python" -c 'import cocotb, cocotbext.axi, cocotb_test' 2>/dev/null; then
    printf 'cocotb, cocotbext-axi or cocotb-test is not installed for %s; install them with\n' \
        "$python" >&2
    printf '  %s -m pip install -r %s\n' "$python" "$here/requirements.txt" >&2
    exit 1
fi

readonly source="$work/taxi-$revision"
"$here/../../fetch-revision.sh" https://github.com/fpganinja/taxi.git "$revision" "$source" || exit 1

COCOTB_PYTHON="$python" "$here/../../../tools/cocotb/makefiles.sh" "$work/makefiles" || exit 1
export PATH="$(dirname "$enzan"):$(dirname "$(command -v "$python")"):$PATH"

expected_testbench() { grep -qx -- "$1" <(grep -v '^#' "$here/expected.txt"); }
expected_case() { grep -qxF -- "$1 $2" <(grep -v '^#' "$here/expected.txt"); }

status=0
for directory in "$source"/src/axis/tb/*/; do
    testbench="$(basename "$directory")"
    if expected_testbench "$testbench"; then
        printf 'SKIP %s (expected.txt)\n' "$testbench"
        continue
    fi

    (
        cd "$directory" || exit 1
        rm -f results.xml
        parameters="$(make -s --no-print-directory SIM=enzan COCOTB_MAKEFILES_DIR="$work/makefiles" \
            --eval 'enzan-parameters: ; @echo $(foreach v,$(filter PARAM_%,$(.VARIABLES)),-G$(subst PARAM_,,$(v))=$($(v)))' \
            enzan-parameters)"
        timeout 1800 make SIM=enzan COCOTB_MAKEFILES_DIR="$work/makefiles" \
            COMPILE_ARGS="$parameters" >"$work/$testbench.log" 2>&1
    )

    results="$directory/results.xml"
    if [[ ! -f "$results" ]]; then
        printf 'FAIL %s: no results (%s)\n' "$testbench" "$work/$testbench.log"
        status=1
        continue
    fi

    passed=0
    skipped=0
    unexpected=0
    while IFS=$'\t' read -r verdict name; do
        if [[ "$verdict" == pass ]]; then
            passed=$((passed + 1))
        elif [[ "$verdict" == skip ]]; then
            # A case the testbench skips for itself, as cocotb counts it.
            skipped=$((skipped + 1))
        elif expected_case "$testbench" "$name"; then
            printf 'XFAIL %s %s (expected.txt)\n' "$testbench" "$name"
        else
            printf 'FAIL %s %s (%s)\n' "$testbench" "$name" "$work/$testbench.log"
            unexpected=$((unexpected + 1))
        fi
    done < <("$python" - "$results" <<'PY'
import sys
import xml.etree.ElementTree as tree
for case in tree.parse(sys.argv[1]).iter("testcase"):
    if case.find("failure") is not None or case.find("error") is not None:
        verdict = "fail"
    elif case.find("skipped") is not None:
        verdict = "skip"
    else:
        verdict = "pass"
    print(verdict + "\t" + case.get("name"))
PY
)
    if [[ $unexpected -ne 0 ]]; then
        status=1
    else
        printf 'PASS %s: %d cases, %d skipped by the testbench\n' "$testbench" "$passed" "$skipped"
    fi
done

exit "$status"
