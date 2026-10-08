#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs PULP's common_cells through testbenches under Enzan (DESIGN_GUIDE.md section 14.5; the
# third exit design of M5).
#
#   run.sh <enzan> <work-directory>
#
# common_cells and the two repositories it depends on are fetched at the revisions in
# revision.txt. Two sets of testbenches run against every source file of common_cells:
#
# - Its own, those that need neither classes nor queues, `randomize` nor `real` variables, none
#   of which M5 has: cc_addr_decode_tb, cc_graycode_tb and cc_sub_per_hash_tb. These check
#   themselves with immediate assertions, which report an error and so make Enzan exit with 3.
# - The ones in tb/ beside this script, for modules its own testbenches leave out or reach only
#   through classes. Each compares a module against a model, says PASS or FAIL, and ends with
#   `$fatal` on a failure.
#
# A testbench passes when Enzan exits with 0 and nothing reported an error. Concurrent
# assertions are SVA, which M5 does not have, so common_cells' own are compiled out with
# ASSERTS_OFF, the switch its sources provide for simulators without them.
#
# The sources and the testbenches are also analysed into a library, and the modules generic in it
# are held to generic.txt by ../generic-units.sh (DESIGN_GUIDE.md section 18, M5.1).

set -uo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <enzan> <work-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$(realpath "$1")"
mkdir -p "$2"
readonly work="$(realpath "$2")"

declare -A checkout
while read -r name url revision; do
    checkout[$name]="$work/$name-$revision"
    "$here/../../fetch-revision.sh" "$url" "$revision" "${checkout[$name]}" || exit 1
done < <(grep -v '^#' "$here/revision.txt")

readonly cells="${checkout[common_cells]}"
readonly sources=(
    "${checkout[tech_cells_generic]}/src/rtl/tc_clk.sv"
    "${checkout[tech_cells_generic]}/src/rtl/tc_sync.sv"
    "${checkout[common_verification]}/src/clk_rst_gen.sv"
    "$cells"/src/*.sv
)

# The clock generator and the clock cells take their delays in picoseconds.
readonly options=(--timescale ps -D ASSERTS_OFF -I "$cells/include")
testbenches=()
run() {
    local testbench="$1" file="$2"
    (cd "$work" && "$enzan" --top "$testbench" "${options[@]}" "${sources[@]}" "$file") \
        >"$work/$testbench.txt" 2>"$work/$testbench.log"
    local status=$?
    if [[ $status -ne 0 ]]; then
        printf 'FAIL %s: Enzan exited with %d (%s)\n' "$testbench" "$status" \
            "$work/$testbench.log"
        return 1
    fi
    printf 'PASS %s\n' "$testbench"
}

status=0
for testbench in cc_addr_decode_tb cc_graycode_tb cc_sub_per_hash_tb; do
    run "$testbench" "$cells/test/$testbench.sv" || status=1
    testbenches+=("$cells/test/$testbench.sv")
done
for file in "$here"/tb/*.sv; do
    run "$(basename "$file" .sv)" "$file" || status=1
    testbenches+=("$file")
done

# Afresh, so that a library left by an earlier run does not answer for this one. An analysis
# into a library keeps every module of its sources, so one holds all of common_cells.
readonly library="$work/forms"
rm -rf "$library"
if "$enzan" lib create "$library" >/dev/null \
    && "$enzan" analyse "${options[@]}" --out-lib "$library" "${sources[@]}" \
        "${testbenches[@]}" >"$work/forms.log" 2>&1; then
    "$here/../generic-units.sh" "$enzan" "$here/generic.txt" "$library" || status=1
else
    printf 'FAIL common_cells was not analysed into a library (%s)\n' "$work/forms.log"
    status=1
fi

exit "$status"
