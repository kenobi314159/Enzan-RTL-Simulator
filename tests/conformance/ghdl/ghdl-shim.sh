#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The `ghdl` GHDL's own test suite calls, standing in for it with Enzan (DESIGN_GUIDE.md section
# 14.4), so that the suite's testsuite.sh scripts run unchanged. ENZAN names the Enzan to run.
#
# Enzan analyses and runs VHDL in one process rather than through libraries on disk, so `-a`
# records the files it is given in the test's directory, in .enzan-shim, after analysing all of
# them so far to report what the test analyses wrong, and `-i` records them unanalysed; `-e` and
# `-m`, `-r` and `--elab-run` elaborate or run a top over every file recorded; and `--remove`
# forgets them. `--expect-failure` asks for the
# step to fail, and inverts what Enzan's exit says. A design across several work libraries, which
# Enzan's one `--work` cannot give, and anything else GHDL does that Enzan does not, fails with a
# line saying so.

set -uo pipefail

readonly state=".enzan-shim"

fail() {
    printf 'enzan shim: %s\n' "$1" >&2
    exit 1
}

# A time in femtoseconds, which a VHDL run counts in: `200ns`, `1 us`, `5000`.
femtoseconds() {
    local text="${1// /}"
    local number="${text%%[a-z]*}" unit="${text##*[0-9]}"
    [[ "$number" =~ ^[0-9]+$ ]] || return 1
    case "$unit" in
    fs) echo "$number" ;;
    ps) echo "$((number * 1000))" ;;
    ns | "") echo "$((number * 1000000))" ;;
    us) echo "$((number * 1000000000))" ;;
    ms) echo "$((number * 1000000000000))" ;;
    sec) echo "$((number * 1000000000000000))" ;;
    *) return 1 ;;
    esac
}

# The revision of the language GHDL's `--std` names, as Enzan's `--std` takes it.
revision() {
    case "$1" in
    87 | 93 | 93c) echo 1993 ;;
    00) echo 2000 ;;
    02) echo 2002 ;;
    08) echo 2008 ;;
    19) echo 2019 ;;
    *) return 1 ;;
    esac
}

[[ $# -ge 1 ]] || fail "no command"
command="$1"
shift

case "$command" in
--version)
    printf 'GHDL 4.1.0 (Enzan conformance shim)\n llvm code generator\n'
    exit 0
    ;;
--remove)
    rm -rf "$state"
    exit 0
    ;;
-a | -s | -i | -e | -m | -r | --elab-run) ;;
*) fail "GHDL's $command is not something Enzan does" ;;
esac

# The options GHDL takes before its operands; what GHDL alone has, fails.
std=1993
work=work
expectFailure=no
operands=()
generics=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --std=*) std="$(revision "${1#--std=}")" || fail "the revision $1" ;;
    -g*=*) generics+=(-G "${1#-g}") ;;
    --work=*) work="${1#--work=}" ;;
    --expect-failure) expectFailure=yes ;;
    --workdir=* | -P* | -O* | -g | -g[0-9] | -W* | --warn-* | -frelaxed* | -fexplicit | -C | \
        --mb-comments | --ieee=standard | --vital-checks | --no-vital-checks | --force-analysis | \
        -d*) ;;
    -fsynopsys | --ieee=synopsys | --ieee=mentor) fail "the vendors' IEEE packages, $1" ;;
    -*) [[ ${#operands[@]} -eq 0 ]] || break; fail "GHDL's option $1" ;;
    *) operands+=("$1") ;;
    esac
    shift
done
runOptions=("$@")

recorded=()
if [[ -f "$state/sources" ]]; then
    while IFS=$'\t' read -r library file; do
        [[ "$library" == "$work" ]] || fail "a design in the libraries $library and $work"
        recorded+=("$file")
    done <"$state/sources"
    [[ -f "$state/std" ]] && std="$(cat "$state/std")"
fi

# The step's own exit, inverted when the test expects it to fail.
finish() {
    local status=$1
    if [[ "$expectFailure" == yes ]]; then
        [[ $status -ne 0 ]] && exit 0
        fail "the step succeeded where the test expects it to fail"
    fi
    exit "$status"
}

case "$command" in
-i)
    mkdir -p "$state"
    echo "$std" >"$state/std"
    for file in "${operands[@]}"; do
        printf '%s\t%s\n' "$work" "$(realpath "$file")" >>"$state/sources"
    done
    exit 0
    ;;
-a | -s)
    [[ ${#operands[@]} -gt 0 ]] || fail "nothing to analyse"
    "$ENZAN" analyse --std "$std" --work "$work" "${recorded[@]}" "${operands[@]}"
    status=$?
    if [[ $status -eq 0 && "$command" == -a && "$expectFailure" == no ]]; then
        mkdir -p "$state"
        echo "$std" >"$state/std"
        for file in "${operands[@]}"; do
            printf '%s\t%s\n' "$work" "$(realpath "$file")" >>"$state/sources"
        done
    fi
    finish "$status"
    ;;
esac

# `-e top [architecture]`, `-r` and `--elab-run` the same followed by the run's own options.
[[ ${#operands[@]} -ge 1 ]] || fail "no top to elaborate"
top="${operands[0]}"
[[ ${#operands[@]} -le 2 ]] || fail "operands after the top: ${operands[*]}"
[[ ${#operands[@]} -eq 1 ]] || fail "choosing the architecture ${operands[1]} of the top"
[[ ${#recorded[@]} -gt 0 ]] || fail "nothing analysed to elaborate"

# Elaboration and a run take their sources from a file list.
printf '%s\n' "${recorded[@]}" >"$state/sources.f"
arguments=(--std "$std" --work "$work" -f "$state/sources.f" "${generics[@]}")
if [[ "$command" == -e || "$command" == -m ]]; then
    "$ENZAN" elaborate "$top" "${arguments[@]}"
    finish $?
fi

for option in "${runOptions[@]}"; do
    case "$option" in
    --stop-time=*)
        time="$(femtoseconds "${option#--stop-time=}")" || fail "the stop time $option"
        arguments+=(--run "$time")
        ;;
    --assert-level=*) arguments+=(--assert-level "${option#--assert-level=}") ;;
    --ieee-asserts=*) arguments+=(--ieee-asserts "${option#--ieee-asserts=}") ;;
    --vcd=*) arguments+=(--vcd "${option#--vcd=}") ;;
    --fst=*) arguments+=(--fst "${option#--fst=}") ;;
    -g*=*) arguments+=(-G "${option#-g}") ;;
    --expect-failure) expectFailure=yes ;;
    --stats | --disp-time | --disp-tree* | --no-run | --stop-delta=* | --max-stack-alloc=* | \
        --unbuffered | --trace-*) ;;
    *) fail "GHDL's run option $option" ;;
    esac
done
"$ENZAN" run "$top" "${arguments[@]}"
finish $?
