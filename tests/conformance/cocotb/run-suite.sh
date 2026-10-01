#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs cocotb's own regression suite against Enzan (DESIGN_GUIDE.md section 14.6).
#
#   run-suite.sh <enzan> <work-directory>
#
# The cocotb used is the one installed for $COCOTB_PYTHON, or for python3; `pip install -r
# requirements.txt` beside this script installs the pinned one. Its source is fetched at the
# matching tag into the work directory, and every directory of tests/test_cases runs with
# SIM=enzan exactly as cocotb's own CI runs it. A case passes when its `make` succeeds, which is
# cocotb's definition.
#
# The pass rate is printed, and passing.txt is a ratchet: the run fails when a case listed there
# no longer passes, and names any case that newly does, to be added in the same commit as the
# change that made it pass.

set -uo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <enzan> <work-directory>\n' "$0" >&2
    exit 1
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
mkdir -p "$2"
readonly work="$(realpath "$2")"
readonly python="${COCOTB_PYTHON:-python3}"
readonly timeout_seconds=600

if ! version="$("$python" -c 'import cocotb; print(cocotb.__version__)' 2>/dev/null)"; then
    printf 'cocotb is not installed for %s; install it with\n  %s -m pip install -r %s\n' \
        "$python" "$python" "$here/requirements.txt" >&2
    exit 1
fi

# The suite at the installed version, fetched once and kept: the tag never moves.
readonly source="$work/cocotb-$version"
if [[ ! -d "$source/tests/test_cases" ]]; then
    rm -rf "$source"
    git clone --quiet --depth 1 --branch "v$version" https://github.com/cocotb/cocotb.git "$source" \
        || { printf 'could not fetch cocotb v%s\n' "$version" >&2; exit 1; }
fi

# A fresh copy of the tests for every run, since running a case writes into its directory. The
# copy keeps the `tests` level, because a case finds its design as ../../../tests/designs.
readonly run="$work/run"
rm -rf "$run"
mkdir -p "$run"
cp -r "$source/tests" "$run/tests"

COCOTB_PYTHON="$python" "$here/../../../tools/cocotb/makefiles.sh" "$work/makefiles"

# The Python cocotb belongs to first, so that `python`, `pytest` and `cocotb-config` in a case's
# Makefile are that one, and the enzan under test ahead of any other.
python_bin="$(dirname "$(command -v "$python")")"
export PATH="$(dirname "$enzan"):$python_bin:$PATH"

passed=()
failed=()
simulated=0
for directory in "$run"/tests/test_cases/*/; do
    name="$(basename "$directory")"
    # Entered rather than named with `make -C`, as cocotb's own tests/Makefile does: the cases
    # read $(PWD), which only the shell keeps up to date. SIM and TOPLEVEL_LANG are in the
    # environment because cocotb's CI puts them there, and some cases read them from it.
    if (cd "$directory" && SIM=enzan TOPLEVEL_LANG=verilog timeout "$timeout_seconds" make \
        COCOTB_MAKEFILES_DIR="$work/makefiles") >"$work/$name.log" 2>&1; then
        passed+=("$name")
        # A case passes by cocotb's definition when its make succeeds, and many succeed without
        # cocotb ever running a test under Enzan: they skip themselves for anything that is not
        # a VHDL simulator or Verilator, or expect the run to fail and see it fail at analysis.
        # Those are counted but said so, because such a pass says nothing about VPI.
        if grep -q "Running on Enzan" "$work/$name.log"; then
            simulated=$((simulated + 1))
            printf 'PASS %s\n' "$name"
        else
            printf 'PASS %s (cocotb did not run under Enzan)\n' "$name"
        fi
    else
        failed+=("$name")
        printf 'FAIL %s (%s)\n' "$name" "$work/$name.log"
    fi
done

total=$((${#passed[@]} + ${#failed[@]}))
printf '\ncocotb %s regression: %d of %d cases pass (%d%%), %d of them with cocotb running\n' \
    "$version" "${#passed[@]}" "$total" $((100 * ${#passed[@]} / total)) "$simulated"

# The ratchet.
status=0
while read -r expected; do
    [[ -z "$expected" || "$expected" == \#* ]] && continue
    if [[ ! " ${passed[*]} " == *" $expected "* ]]; then
        printf 'REGRESSED %s, which passing.txt lists as passing\n' "$expected"
        status=1
    fi
done <"$here/passing.txt"

for name in "${passed[@]}"; do
    if ! grep -qx "$name" "$here/passing.txt"; then
        printf 'NEW %s passes; add it to %s\n' "$name" "$here/passing.txt"
    fi
done

exit "$status"
