#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The `ghdl` GHDL's own test suite calls, standing in for it with Enzan, so that the suite's
# testsuite.sh scripts run unchanged. ENZAN names the Enzan to run.
#
# Each GHDL library is an Enzan library in the test's directory, .enzan-shim/libs/<name>: `-a`
# analyses its files into the library `--work` names, against every other, as GHDL does; `-i`
# records them, to be analysed so before the next elaboration; `-e` and `-m`, `-r` and
# `--elab-run` elaborate or run a top from the libraries, `lib.top` or a top of the `--work`
# library; and `--remove` removes the `--work` library. `--expect-failure` asks for the step to
# fail, and inverts what Enzan's exit says. Anything GHDL does that Enzan does not fails with a
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
-a | -s | -i | -e | -m | -r | --elab-run | --remove) ;;
*) fail "GHDL's $command is not something Enzan does" ;;
esac

# The options GHDL takes before its operands; what GHDL alone has, fails.
std=1993
stdGiven=no
work=work
expectFailure=no
operands=()
generics=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --std=*)
        std="$(revision "${1#--std=}")" || fail "the revision $1"
        stdGiven=yes
        ;;
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

library=$(tr '[:upper:]' '[:lower:]' <<<"$work")
readonly libraries="$state/libs"
# A step naming no revision takes the one the design was analysed in.
[[ "$stdGiven" == no && -f "$state/std" ]] && std="$(cat "$state/std")"

# `--in-lib` for every library there is but `except`.
others() {
    local except="$1" directory
    for directory in "$libraries"/*/; do
        [[ -d "$directory" ]] || continue
        directory="${directory%/}"
        [[ "$(basename "$directory")" == "$except" ]] || printf '%s\n' --in-lib "$directory"
    done
}

# Analyses files into the library named `$1`, creating it the first time, against every other.
analyse_into() {
    local into="$1"
    shift
    mkdir -p "$libraries"
    if [[ ! -d "$libraries/$into" ]]; then
        "$ENZAN" lib create --name "$into" "$libraries/$into" >/dev/null || return 1
    fi
    local against=()
    mapfile -t against < <(others "$into")
    "$ENZAN" analyse --std "$std" --out-lib "$libraries/$into" "${against[@]}" "$@"
}

# The files `-i` recorded, analysed into their libraries, before anything elaborates.
analyse_imported() {
    local list name files=()
    for list in "$state"/imported/*.f; do
        [[ -f "$list" ]] || continue
        name="$(basename "$list" .f)"
        mapfile -t files <"$list"
        analyse_into "$name" "${files[@]}" || return 1
        rm -f "$list"
    done
}

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
--remove)
    rm -rf "${libraries:?}/$library" "$state/imported/$library.f"
    exit 0
    ;;
-i)
    mkdir -p "$state/imported"
    echo "$std" >"$state/std"
    for file in "${operands[@]}"; do realpath "$file" >>"$state/imported/$library.f"; done
    exit 0
    ;;
-a)
    [[ ${#operands[@]} -gt 0 ]] || fail "nothing to analyse"
    mkdir -p "$state"
    echo "$std" >"$state/std"
    analyse_into "$library" "${operands[@]}"
    finish $?
    ;;
-s)
    # Checked, and stored nowhere: analysed into a library of the name that holds nothing.
    [[ ${#operands[@]} -gt 0 ]] || fail "nothing to analyse"
    mapfile -t against < <(others "$library")
    "$ENZAN" analyse --std "$std" --work "$library" "${against[@]}" "${operands[@]}"
    finish $?
    ;;
esac

# `-e top [architecture]`, `-r` and `--elab-run` the same followed by the run's own options.
[[ ${#operands[@]} -ge 1 ]] || fail "no top to elaborate"
top="${operands[0]}"
[[ ${#operands[@]} -le 2 ]] || fail "operands after the top: ${operands[*]}"
[[ ${#operands[@]} -eq 1 ]] || fail "choosing the architecture ${operands[1]} of the top"
analyse_imported || finish 1
compgen -G "$libraries/*/" >/dev/null || fail "nothing analysed to elaborate"

mapfile -t against < <(others "")
arguments=(--std "$std" --work "$library" "${against[@]}" "${generics[@]}")
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
