#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A mixed-language library, with each language as the top in turn. The sources are hidden before
# the runs so that success proves the linked EIR specialisation, rather than a new frontend pass,
# crossed the process boundary.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <enzan> <source-directory>" >&2
    exit 2
fi

enzan=$1
source_directory=$2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$source_directory/top_overrides.vhd" "$source_directory/top_overrides.sv" "$work/"
cd "$work"

fail() {
    echo "FAIL mixed library: $1" >&2
    exit 1
}

status_of() {
    local status=0
    "$@" >/dev/null 2>&1 || status=$?
    echo "$status"
}

"$enzan" analyse --out-lib vhdl_top --top tb -GDEPTH=5 \
    top_overrides.vhd top_overrides.sv \
    || fail "the VHDL-top design could not be stored"
"$enzan" analyse --out-lib sv_top --top sv_tb -GWIDTH=3 \
    top_overrides.vhd top_overrides.sv \
    || fail "the SystemVerilog-top design could not be stored"

vhdl_listing=$("$enzan" lib list vhdl_top)
grep -q 'tb' <<<"$vhdl_listing" && grep -q 'leaf' <<<"$vhdl_listing" \
    || fail "one language is absent from the VHDL-top library: $vhdl_listing"
sv_listing=$("$enzan" lib list sv_top)
grep -q 'sv_tb' <<<"$sv_listing" && grep -qi 'vleaf' <<<"$sv_listing" \
    || fail "one language is absent from the SystemVerilog-top library: $sv_listing"

mv top_overrides.vhd top_overrides.vhd.saved
mv top_overrides.sv top_overrides.sv.saved

vhdl_run=$("$enzan" run --in-lib vhdl_top -GDEPTH=5 tb 2>&1) \
    || fail "the VHDL-top specialisation did not run: $vhdl_run"
grep -q 'PASS top_overrides' <<<"$vhdl_run" \
    || fail "the VHDL-top design did not reach its check: $vhdl_run"

sv_run=$("$enzan" run --in-lib sv_top -GWIDTH=3 sv_tb 2>&1) \
    || fail "the SystemVerilog-top specialisation did not run: $sv_run"
grep -q 'PASS sv_tb' <<<"$sv_run" \
    || fail "the SystemVerilog-top design did not reach its check: $sv_run"

# Overrides are part of the EIR specialisation key. With the sources hidden, another value must
# miss rather than accidentally running the one already stored.
[[ $(status_of "$enzan" run --in-lib vhdl_top -GDEPTH=6 tb) -eq 2 ]] \
    || fail "a different generic reused the stored specialisation"

echo "PASS mixed library"
