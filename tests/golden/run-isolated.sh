#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a design under Enzan and under Icarus Verilog, each in an empty directory of its own, and
# compares what each printed and every file each wrote: for designs whose result is files, such
# as sv/files.sv, or that read files they write first, such as sv/readmem.sv.
#
#   run-isolated.sh <enzan> <iverilog> <source.sv> <work-directory>
#
# Prints `PASS <name>`, the source's name without its extension, when the two agree.

set -u
enzan="$1"
iverilog="$2"
source="$3"
work="$4"
name="$(basename "$source" .sv)"
vvp="$(dirname "$(command -v "$iverilog")")/vvp"

rm -rf "$work"
mkdir -p "$work/enzan" "$work/reference"
(cd "$work/enzan" && "$enzan" --top tb "$source" >transcript.out 2>"$work/enzan.log") || {
    echo "FAIL: Enzan did not run the design ($work/enzan.log)"
    exit 1
}
"$iverilog" -g2012 -o "$work/design.vvp" "$source" || exit 1
# Icarus Verilog's own warnings and its `$finish` line are on its standard output, and are not
# the design's.
(cd "$work/reference" \
    && "$vvp" -n "$work/design.vvp" | grep -v -e 'finish called at' -e '^WARNING: ' >transcript.out)

if ! diff -r "$work/reference" "$work/enzan"; then
    echo "FAIL: Enzan's output differs from Icarus Verilog's"
    exit 1
fi
echo "PASS $name"
