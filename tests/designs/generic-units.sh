#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The share of a design's parameterised modules that are generic units, and the ratchet it is
# held to (DESIGN_GUIDE.md section 18, M5.1).
#
#   generic-units.sh <enzan> <generic-list> <library>...
#
# Each <library> holds an analysis of the design, typically one per testbench. A module is
# generic when every library that holds it holds one generic unit of it, and specialised when
# any holds a parameterisation of it instead; modules with no parameters of their own are not
# counted. The share is printed, and the generic modules are held to <generic-list> by
# ../conformance/ratchet.sh: one that stops being generic fails the run, and one that becomes
# generic is to be added to the list in the same commit as the change that made it.

set -uo pipefail

if [[ $# -lt 3 ]]; then
    printf 'usage: %s <enzan> <generic-list> <library>...\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly enzan="$1"
readonly list="$2"
shift 2

listing="$(mktemp)"
generic="$(mktemp)"
trap 'rm -f "$listing" "$generic" "$generic.all" "$generic.specialised"' EXIT

for library in "$@"; do
    "$enzan" lib list "$library" >>"$listing" || exit 1
done

# `lib list` marks a generic unit `name (generic)`, and a parameterisation
# `mangled (a parameterisation of name)`.
sed -n 's/^  \([^ ]*\) (generic)$/\1/p' "$listing" | sort -u >"$generic.all"
sed -n 's/^  [^ ]* (a parameterisation of \([^ )]*\))$/\1/p' "$listing" | sort -u >"$generic.specialised"
comm -23 "$generic.all" "$generic.specialised" >"$generic"
total=$(sort -u "$generic.all" "$generic.specialised" | wc -l)
count=$(wc -l <"$generic")

if [[ $total -eq 0 ]]; then
    printf 'GENERIC no parameterised modules\n'
else
    printf 'GENERIC %d of %d parameterised modules (%d%%)\n' "$count" "$total" \
        $((count * 100 / total))
fi
"$here/../conformance/ratchet.sh" "$list" "$generic" generic
