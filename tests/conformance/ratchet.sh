#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ratchet a conformance suite's pass list is held to.
#
#   ratchet.sh <passing-list> <passed>
#
# <passing-list> names, one per line, every case that has passed before; `#` starts a comment.
# <passed> names every case that passed in this run. The ratchet fails when a case on the list
# did not pass, and names each case that newly did, to be added to the list in the same commit
# as the change that made it pass. A pass rate alone would let one case start passing while
# another stopped and report no change at all, which is why the list is the ratchet and not
# the number.

set -uo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <passing-list> <passed>\n' "$0" >&2
    exit 2
fi

readonly list="$1"
readonly passed="$2"

# `|| [[ -n ... ]]` so that a last line with no newline after it is read too.
status=0
while read -r expected || [[ -n "$expected" ]]; do
    [[ -z "$expected" || "$expected" == \#* ]] && continue
    if ! grep -qxF -- "$expected" "$passed"; then
        printf 'REGRESSED %s, which %s lists as passing\n' "$expected" "$list"
        status=1
    fi
done <"$list"

while read -r name || [[ -n "$name" ]]; do
    [[ -z "$name" ]] && continue
    if ! grep -qxF -- "$name" "$list"; then
        printf 'NEW %s passes; add it to %s\n' "$name" "$list"
    fi
done <"$passed"

exit "$status"
