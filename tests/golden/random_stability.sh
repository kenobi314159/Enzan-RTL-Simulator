#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Random stability and the seed (DESIGN_GUIDE.md section 3.8): one seed gives the same run twice,
# another seed gives other draws, and one process's $urandom sequence does not depend on whether
# another process draws too.
#
#   random_stability.sh <enzan> <random_stability.sv> <work-directory>

set -u
enzan="$1"
source="$2"
mkdir -p "$3"
work="$3"

run() {
    "$enzan" --top tb "$@" "$source" 2>/dev/null | grep urandom
}

run --seed 7 >"$work/seed7.txt" || exit 1
run --seed 7 >"$work/seed7-again.txt" || exit 1
run --seed 8 >"$work/seed8.txt" || exit 1
run --seed 7 -G Extra=1 >"$work/seed7-extra.txt" || exit 1

status=0
if ! cmp -s "$work/seed7.txt" "$work/seed7-again.txt"; then
    echo "FAIL: one seed drew different numbers in two runs"
    status=1
fi
if cmp -s "$work/seed7.txt" "$work/seed8.txt"; then
    echo "FAIL: two seeds drew the same numbers"
    status=1
fi
if ! cmp -s "$work/seed7.txt" "$work/seed7-extra.txt"; then
    echo "FAIL: another process drawing changed what this one drew"
    status=1
fi
[ "$status" -eq 0 ] && echo "PASS random_stability"
exit "$status"
