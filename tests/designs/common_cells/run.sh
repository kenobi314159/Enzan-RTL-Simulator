#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs PULP's common_cells through testbenches under Enzan.
#
#   run.sh <enzan> <work-directory>
#
# common_cells and the two repositories it depends on are fetched at the revisions in
# revision.txt. Two sets of testbenches run against every source file of common_cells:
#
# - Its own, those that need neither classes nor queues, `randomize` nor `real` variables, none
#   of which Enzan has: cc_addr_decode_tb, cc_graycode_tb and cc_sub_per_hash_tb. These check
#   themselves with immediate assertions, which report an error and so make Enzan exit with 3.
# - The ones in tb/ beside this script, for modules its own testbenches leave out or reach only
#   through classes. Each compares a module against a model, says PASS or FAIL, and ends with
#   `$fatal` on a failure.
#
# A testbench passes when Enzan exits with 0 and nothing reported an error. Concurrent
# assertions are SVA, which Enzan does not have, so common_cells' own are compiled out with
# ASSERTS_OFF, the switch its sources provide for simulators without them.

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
run() {
    local testbench="$1" file="$2"
    (cd "$work" && "$enzan" --top "$testbench" --timescale ps -D ASSERTS_OFF \
        -I "$cells/include" "${sources[@]}" "$file") \
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
done
for file in "$here"/tb/*.sv; do
    run "$(basename "$file" .sv)" "$file" || status=1
done

exit "$status"
