#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Validates FST output by reading it back with GTKWave's own converter.
#
# A waveform has to open in GTKWave, in VCD and in FST alike. A viewer needs a display and cannot
# run in CI, but `fst2vcd` is the same reader GTKWave uses, so putting Enzan's FST through it and
# comparing the result against Enzan's own VCD checks three things at once:
#
#   * the FST is a file GTKWave can read at all;
#   * its hierarchy survives the round trip, aliases included — a collapsed port is one variable
#     under two names, and a writer that got that wrong would duplicate the storage;
#   * the two writers agree about every value at every timestamp, so a bug in either shows up as a
#     difference rather than as a plausible-looking waveform.
#
# Usage: run-fst-check.sh <enzan> <fst2vcd> <checker> <source.sv> <top> [<enzan option>...]
#
# The options are those the case's golden test runs it with, as run-vcd-check.sh takes them.

set -euo pipefail

if [[ $# -lt 5 ]]; then
    printf 'usage: %s <enzan> <fst2vcd> <checker> <source.sv> <top> [<enzan option>...]\n' \
        "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly fst2vcd="$2"
readonly checker="$3"
readonly source_file="$4"
readonly top="$5"
shift 5

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Both formats from one run, so that any difference is in the writers rather than in two
# simulations that might legitimately differ.
"$enzan" --top "$top" "$@" --vcd "$workdir/direct.vcd" --fst "$workdir/wave.fst" \
    "$source_file" >/dev/null

if [[ ! -s "$workdir/wave.fst" ]]; then
    printf 'no FST was written\n' >&2
    exit 1
fi

"$fst2vcd" "$workdir/wave.fst" -o "$workdir/roundtrip.vcd" >"$workdir/convert.log" 2>&1 || {
    printf 'GTKWave could not read the FST:\n' >&2
    cat "$workdir/convert.log" >&2
    exit 1
}

# The round-tripped file must be well formed in its own right, not merely non-empty.
"$checker" "$workdir/roundtrip.vcd"

# Each value change is compared with the timestamp it belongs to, as an unordered set.
#
# VCD does not define an order for the changes within one timestamp, and the two paths legitimately
# differ: Enzan writes them in the order the kernel reported them, while the FST reader emits them
# in its own. Comparing the text line by line would fail on that and say nothing about correctness.
# Keying each change by its time catches everything that matters — a value that differs, a change at
# the wrong timestamp, a change that only one path emitted — while ignoring what does not.
#
# Keyword lines are dropped: the headers differ legitimately, and fst2vcd need not bracket its
# initial dump the way Enzan does.
values() {
    # The header is dropped first: its free-text lines — a version string, a date — do not begin
    # with `$`, so the value-change scanner below would otherwise mistake them for changes.
    sed -n '/\$enddefinitions/,$p' "$1" | awk '
        /^\$/ { next }
        /^#/   { time = substr($0, 2); next }
        NF     { print time "\t" $0 }
    ' | sort
}

if ! diff -u <(values "$workdir/direct.vcd") <(values "$workdir/roundtrip.vcd"); then
    printf '\nThe FST and the VCD disagree about %s.\n' "$(basename "$source_file")" >&2
    printf 'The left side is the VCD written directly, the right side is the FST read back.\n' >&2
    exit 1
fi

printf '%s: FST round-trips through GTKWave and matches the VCD.\n' "$(basename "$source_file")"
