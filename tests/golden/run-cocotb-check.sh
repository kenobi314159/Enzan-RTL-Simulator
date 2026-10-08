#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the cocotb golden test against Enzan, through cocotb's own makefiles with SIM=enzan.
#
#   run-cocotb-check.sh <enzan> <python-with-cocotb> <test-directory>
#
# Prints `PASS cocotb` when every test in it passed.

set -euo pipefail

readonly enzan="$(realpath "$1")"
readonly python="$2"
readonly source="$(realpath "$3")"
readonly root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

COCOTB_PYTHON="$python" "$root/tools/cocotb/makefiles.sh" "$work/makefiles"
cp -r "$source" "$work/case"

# The Python cocotb belongs to first, so that `cocotb-config` is that one's.
export PATH="$(dirname "$(command -v "$python")"):$PATH"

cd "$work/case"
SIM=enzan TOPLEVEL_LANG=verilog make COCOTB_MAKEFILES_DIR="$work/makefiles" \
    ENZAN_BIN_DIR="$(dirname "$enzan")"
echo "PASS cocotb"
