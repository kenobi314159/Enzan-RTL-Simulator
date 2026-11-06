#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes NEORV32's functional-simulation netlists, which ../run.sh simulates when XILINX_NETLISTS
# names the directory they are in, by running Vivado.
#
#   make.sh <output-directory>
#
# Vivado is VIVADO, or the one in XILINX_VIVADO's bin, or the one on the path. NEORV32 is fetched at
# the revision ../../neorv32/revision.txt names, the design regression's, and configured as its
# testbench is (wrap.py). Everything is written to <output-directory>, the netlists among it, with
# the wrappers they are made from and Vivado's log. README.md says what a distribution AMD does not
# support needs.

set -uo pipefail

if [[ $# -ne 1 ]]; then
    printf 'usage: %s <output-directory>\n' "$0" >&2
    exit 2
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$1"
readonly out="$(realpath "$1")"
vivado="${VIVADO:-}"
[[ -z "$vivado" && -n "${XILINX_VIVADO:-}" ]] && vivado="$XILINX_VIVADO/bin/vivado"
[[ -z "$vivado" ]] && vivado="$(command -v vivado)"
if [[ -z "$vivado" || ! -x "$vivado" ]]; then
    printf 'make.sh: no Vivado; set VIVADO or XILINX_VIVADO\n' >&2
    exit 1
fi

readonly revision="$(grep -v '^#' "$here/../../neorv32/revision.txt" | head -1)"
readonly source="$out/neorv32-$revision"
"$here/../../../fetch-revision.sh" https://github.com/stnolting/neorv32.git "$revision" "$source" \
    || exit 1

"$here/wrap.py" "$source" "$here/../../neorv32/tb/neorv32_tb.v" "$out/wrappers" || exit 1
(cd "$out" && "$vivado" -mode batch -nojournal -log "$out/vivado.log" -source "$here/synth.tcl" \
    -tclargs "$source" "$out/wrappers" "$out") >/dev/null || {
    printf 'make.sh: Vivado failed (%s)\n' "$out/vivado.log" >&2
    exit 1
}
printf 'made %s and %s\n' "$out/neorv32_funcsim.v" "$out/neorv32_funcsim.vhd"
