#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the VESTs conformance suite (DESIGN_GUIDE.md section 14.4), as it ships inside GHDL's test
# suite, under Enzan.
#
#   run-suite.sh <enzan> <work-directory>
#
# GHDL's sources are fetched at the revision in revision.txt. Every compliant case its VHDL-93
# `.exp` files run, `run_compliant_test`, is run by those files' own rules, as GHDL's
# testsuite.sh reads them: analysed as VHDL-93 together with the units each `.exp` builds before
# it, elaborated from the top its ENTITY= names or else the last entity named as a top is
# (`...ent`, `...entw`), stopped at its STOP= time, and run in its directory, where its files are,
# with --assert-level error as GHDL runs it. A case passes when Enzan runs it to its end and it
# reports no FAILED TEST; most report PASSED TEST, and every one checks itself. A case whose
# units belong to a library other than work, LIBRARY=, is run as the rest are and fails until
# Enzan analyses several libraries in one run.
#
# A case that writes a file another reads, OUTPUT=, runs before every case that reads one, INPUT=,
# as GHDL's suite runs them in order: each simulator reads back the files it wrote, in its own
# format, and the files the suite commits are another tool's.
#
# Prints PASS or FAIL for each case, then the counts, and writes the cases that passed to
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
readonly revision="$(grep -v '^#' "$here/revision.txt" | head -1)"
readonly source="$work/ghdl-$revision"
"$here/../../fetch-revision.sh" https://github.com/ghdl/ghdl.git "$revision" "$source" || exit 1
readonly vests="$source/testsuite/vests/vhdl-93"

# What an earlier run's cases wrote beside them would be read as this run's.
git -C "$source" clean -fdxq testsuite

# One line per case: its name, its directory, its top, its stop time, then its sources, the
# units built before it first. Names are the case's path under vhdl-93, which the pass list holds.
# The cases that write files are listed apart, to run first.
cases="$work/cases.txt"
writers="$work/writers.txt"
: >"$cases"
: >"$writers"
while read -r exp; do
    directory="$(dirname "$exp")"
    built=()
    while read -r kind file options; do
        case "$kind" in
        build_compliant_test) built+=("$directory/$file") ;;
        run_compliant_test)
            entity=""
            stop=""
            list="$cases"
            for option in $options; do
                case "$option" in
                ENTITY=*) entity="${option#ENTITY=}" ;;
                STOP=*) stop="${option#STOP=}" ;;
                OUTPUT=*) list="$writers" ;;
                esac
            done
            if [[ -z "$entity" ]]; then
                entity="$(sed -n -E -e '/^ENTITY .*(ent|entw) IS$/s/ENTITY (.*) IS/\1/p' \
                    "$directory/$file" | tail -1)"
            fi
            printf '%s\t%s\t%s\t%s\t%s\n' "${directory#"$vests"/}/$file" "$directory" \
                "${entity:-none}" "${stop:-none}" "${built[*]} $directory/$file" >>"$list"
            ;;
        esac
    done < <(grep -E '^(build|run)_compliant_test ' "$exp")
done < <(find "$vests" -path '*non_compliant*' -prune -o -name '*.exp' -print | sort)

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

run_case() {
    local name directory entity stop files
    IFS=$'\t' read -r name directory entity stop files <<<"$1"
    local log="$WORK/logs/${name//\//_}.log"
    local arguments=(--std 1993 --assert-level error)
    [[ "$entity" != none ]] && arguments+=(--top "$entity")
    if [[ "$stop" != none ]]; then
        local limit
        limit="$(femtoseconds "$stop")"
        [[ -n "$limit" ]] && arguments+=(--run "$limit")
    fi
    # shellcheck disable=SC2086 # the sources are words
    (cd "$directory" && timeout 120 "$ENZAN" "${arguments[@]}" $files) >"$log" 2>&1
    local status=$?
    if [[ $status -eq 0 ]] && ! grep -q 'FAILED TEST' "$log"; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s\n' "$name"
    fi
}
export -f run_case

mkdir -p "$work/logs"
export ENZAN="$enzan" WORK="$work"
{
    for list in "$writers" "$cases"; do
        tr '\n' '\0' <"$list" | xargs -0 -P "$(nproc)" -I{} bash -c 'run_case "$1"' _ {}
    done
} | sort -k2 >"$work/results.txt"
cat "$work/results.txt"

grep '^PASS ' "$work/results.txt" | cut -d' ' -f2 >"$work/passed.txt"
passed="$(wc -l <"$work/passed.txt")"
total="$(wc -l <"$work/results.txt")"
printf '\nVESTs: %d of %d compliant cases pass (%d.%d%%)\n' "$passed" "$total" \
    "$((passed * 100 / total))" "$((passed * 1000 / total % 10))"

"$here/../ratchet.sh" "$here/passing.txt" "$work/passed.txt"
