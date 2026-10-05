#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# File output (sv/files.sv): runs the design under Enzan and under Icarus Verilog, each in a
# directory of its own, and compares what each printed and every file each wrote.
#
#   files.sh <enzan> <iverilog> <files.sv> <work-directory>

set -u
enzan="$1"
iverilog="$2"
source="$3"
work="$4"
vvp="$(dirname "$(command -v "$iverilog")")/vvp"

rm -rf "$work"
mkdir -p "$work/enzan" "$work/reference"
(cd "$work/enzan" && "$enzan" --top tb "$source" >transcript.out 2>enzan.log) || {
    echo "FAIL: Enzan did not run the design ($work/enzan/enzan.log)"
    exit 1
}
"$iverilog" -g2012 -o "$work/files.vvp" "$source" || exit 1
(cd "$work/reference" && "$vvp" -n "$work/files.vvp" | grep -v 'finish called at' >transcript.out)
rm -f "$work/enzan/enzan.log"

if ! diff -r "$work/reference" "$work/enzan"; then
    echo "FAIL: Enzan's output differs from Icarus Verilog's"
    exit 1
fi
echo "PASS files"
