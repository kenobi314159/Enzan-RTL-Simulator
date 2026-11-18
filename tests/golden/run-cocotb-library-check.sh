#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises the incremental library flow of cocotb's Makefile.enzan:
#
#   run-cocotb-library-check.sh <enzan> <python> <case> <language> \
#       <main-dependency> <auxiliary-source>

set -euo pipefail

if [[ $# -ne 6 ]]; then
    printf 'usage: %s <enzan> <python> <case> <language> <main-dependency> <auxiliary-source>\n' \
        "$0" >&2
    exit 2
fi

readonly enzan="$(realpath "$1")"
readonly python="$2"
readonly source="$(realpath "$3")"
readonly language="$4"
readonly main_dependency="$5"
readonly auxiliary_source="$6"
readonly root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

COCOTB_PYTHON="$python" "$root/tools/cocotb/makefiles.sh" "$work/makefiles"
cp -r "$source" "$work/case"

# The Python cocotb belongs to first, so that `cocotb-config` is that one's.
export PATH="$(dirname "$(command -v "$python")"):$PATH"

cd "$work/case"

fail() {
    printf 'FAIL cocotb library: %s\n' "$1" >&2
    exit 1
}

run_make() {
    SIM=enzan TOPLEVEL_LANG="$language" make regression \
        COCOTB_MAKEFILES_DIR="$work/makefiles" ENZAN_BIN_DIR="$(dirname "$enzan")" 2>&1
}

count_analyses() {
    grep -cE '(^|[[:space:]])analyse([[:space:]]|$)' <<<"$1" || true
}

# A cold regression builds the auxiliary and main libraries and then runs from them.
output="$(run_make)" || { printf '%s\n' "$output" >&2; fail "the cold build failed"; }
[[ "$(count_analyses "$output")" -eq 2 ]] \
    || fail "the cold build did not analyse both libraries: $output"

# A warm regression neither analyses nor simulates.
output="$(run_make)" || { printf '%s\n' "$output" >&2; fail "the warm build failed"; }
[[ "$(count_analyses "$output")" -eq 0 ]] \
    || fail "an unchanged design was analysed again: $output"

# The SystemVerilog case names an included header here, not an explicit Make prerequisite. Its
# depfile has to make this rebuild the main library. The VHDL case names its main source directly.
touch "$main_dependency"
output="$(run_make)" || { printf '%s\n' "$output" >&2; fail "the main rebuild failed"; }
[[ "$(count_analyses "$output")" -eq 1 ]] \
    || fail "a main-design change did not rebuild exactly its library: $output"

# An auxiliary change rebuilds that library and the main library analysed against it.
touch "$auxiliary_source"
output="$(run_make)" || { printf '%s\n' "$output" >&2; fail "the dependent rebuild failed"; }
[[ "$(count_analyses "$output")" -eq 2 ]] \
    || fail "an auxiliary change did not rebuild it and the main library: $output"

# cocotb's sim target removes the results and runs again, but current libraries stay current.
output="$(SIM=enzan TOPLEVEL_LANG="$language" make sim \
    COCOTB_MAKEFILES_DIR="$work/makefiles" ENZAN_BIN_DIR="$(dirname "$enzan")" 2>&1)" \
    || { printf '%s\n' "$output" >&2; fail "the forced simulation failed"; }
[[ "$(count_analyses "$output")" -eq 0 ]] \
    || fail "make sim analysed current libraries again: $output"

grep -qE '(^|[[:space:]])run([[:space:]]|$)' <<<"$output" \
    || fail "make sim did not run explicitly from the libraries: $output"

echo "PASS cocotb library $language"
