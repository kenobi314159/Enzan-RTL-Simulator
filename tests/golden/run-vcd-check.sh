#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs a design with waveform output and validates the resulting VCD.
#
# Usage: run-vcd-check.sh <enzan> <checker> <source.sv> <top> [<enzan option>...]
#
# The options are those the case's golden test runs it with, so that the waveform is of the run
# that passes.

set -euo pipefail

if [[ $# -lt 4 ]]; then
    printf 'usage: %s <enzan> <checker> <source.sv> <top> [<enzan option>...]\n' "$0" >&2
    exit 2
fi

readonly enzan="$1"
readonly checker="$2"
readonly source_file="$3"
readonly top="$4"
shift 4

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

"$enzan" --top "$top" "$@" --vcd "$workdir/wave.vcd" "$source_file" >/dev/null
"$checker" "$workdir/wave.vcd"
