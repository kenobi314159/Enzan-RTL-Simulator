#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs NVC's own regression tests (DESIGN_GUIDE.md section 14.4) under Enzan, from the NVC the
# VHDL frontend is built on, at the revision it is vendored at.
#
#   run-suite.sh <enzan> <work-directory>
#
# Every test test/regress/testlist.txt lists is run by its options as NVC's run_regr.c reads them:
# the test's own file, its entity as the top, VHDL-1993 unless an option names another revision,
# its stop= time; it passes when Enzan's exit says what `fail` says it should, and, for a `gold`
# test, when each line of its gold file is found in what Enzan printed, in order. NVC writes a
# message's time and delta in front of it, `1ns+2: message`, which Enzan does not, so the message
# is matched without them. A test of something only NVC has, VHPI, its shell, PSL, coverage,
# waveform dumps, Verilog or its relaxed rules, is in neither count; nor is one setting generics or
# environment variables, which Enzan's command line does not give a VHDL top yet.
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
readonly regress="$(realpath "$here/../../../third_party/nvc/test/regress")"

# One line per test: its name, the options Enzan takes for it, whether it is to fail, and whether
# it has a gold file.
tests="$work/tests.txt"
: >"$tests"
while read -r name options || [[ -n "$name" ]]; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    arguments="--std 1993"
    fail=no
    gold=no
    applicable=yes
    IFS=',' read -r -a flags <<<"$options"
    for flag in "${flags[@]}"; do
        case "$flag" in
        normal | slow | '!windows' | '!freebsd' | shuffle | per-file | no-collapse | O*) ;;
        gold) gold=yes ;;
        fail) fail=yes ;;
        2000 | 2002 | 2008 | 2019) arguments="${arguments/1993/$flag}" ;;
        stop=*) arguments+=" --stop ${flag#stop=}" ;;
        *) applicable=no ;;
        esac
    done
    [[ "$applicable" == yes && -f "$regress/$name.vhd" ]] || continue
    printf '%s\t%s\t%s\t%s\n' "$name" "$arguments" "$fail" "$gold" >>"$tests"
done <"$regress/testlist.txt"

# A stop time in Enzan's femtoseconds, which a VHDL run counts in.
femtoseconds() {
    local number="${1%%[a-z]*}" unit="${1##*[0-9]}"
    case "$unit" in
    fs) echo "$number" ;;
    ps) echo "$((number * 1000))" ;;
    ns) echo "$((number * 1000000))" ;;
    us) echo "$((number * 1000000000))" ;;
    ms) echo "$((number * 1000000000000))" ;;
    *) echo "" ;;
    esac
}
export -f femtoseconds

run_test() {
    local name options fail gold
    IFS=$'\t' read -r name options fail gold <<<"$1"
    local directory="$WORK/run/$name"
    rm -rf "$directory" && mkdir -p "$directory"
    local arguments=()
    read -r -a words <<<"$options"
    local index=0
    while [[ $index -lt ${#words[@]} ]]; do
        if [[ "${words[$index]}" == --stop ]]; then
            local limit
            limit="$(femtoseconds "${words[$((index + 1))]}")"
            [[ -n "$limit" ]] && arguments+=(--run "$limit")
            index=$((index + 2))
            continue
        fi
        arguments+=("${words[$index]}")
        index=$((index + 1))
    done
    local log="$directory/output.txt"
    (cd "$directory" && timeout 60 "$ENZAN" "${arguments[@]}" --top "$name" \
        "$REGRESS/$name.vhd") >"$log" 2>&1
    local status=$?
    local passed=yes
    if [[ "$fail" == yes ]]; then
        [[ $status -ne 0 && $status -ne 124 ]] || passed=no
    else
        [[ $status -eq 0 ]] || passed=no
    fi
    if [[ "$passed" == yes && "$gold" == yes ]]; then
        # Each gold line, without NVC's time and delta, in Enzan's output, in order.
        local remaining message
        remaining="$(cat "$log")"
        while IFS= read -r message; do
            [[ -z "$message" ]] && continue
            local after="${remaining#*"$message"}"
            if [[ "$after" == "$remaining" ]]; then
                passed=no
                break
            fi
            remaining="$after"
        done < <(sed -E 's/^[0-9]+[a-z]*\+[0-9]+: //' "$REGRESS/gold/$name.txt")
    fi
    printf '%s %s\n' "$([[ "$passed" == yes ]] && echo PASS || echo FAIL)" "$name"
}
export -f run_test

export ENZAN="$enzan" WORK="$work" REGRESS="$regress"
tr '\n' '\0' <"$tests" | xargs -0 -P "$(nproc)" -I{} bash -c 'run_test "$1"' _ {} \
    | sort -k2 >"$work/results.txt"
cat "$work/results.txt"

grep '^PASS ' "$work/results.txt" | cut -d' ' -f2 >"$work/passed.txt"
passed="$(wc -l <"$work/passed.txt")"
total="$(wc -l <"$work/results.txt")"
printf '\nNVC regression: %d of %d tests pass (%d.%d%%)\n' "$passed" "$total" \
    "$((passed * 100 / total))" "$((passed * 1000 / total % 10))"

"$here/../ratchet.sh" "$here/passing.txt" "$work/passed.txt"
