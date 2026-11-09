#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes a copy of cocotb's makefile directory, with Enzan's simulator makefile added, into the
# directory given. Then:
#
#   make SIM=enzan COCOTB_MAKEFILES_DIR=<directory>
#
# cocotb chooses a simulator by the name of a makefile in its own directory and offers no way to
# add one from outside. The copy is made of links rather than of files, so it follows the
# cocotb it was made from, and nothing is written into that cocotb's installation.
#
# A cocotb with a Makefile.enzan of its own keeps it: the one beside this script is for cocotb 2.1,
# and loads cocotb in a way later releases removed.
#
# The cocotb used is the one `cocotb-config` belongs to, or the one the Python named by
# COCOTB_PYTHON has installed.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    printf 'usage: %s <directory>\n' "$0" >&2
    exit 1
fi

readonly here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly target="$1"

if [[ -n "${COCOTB_PYTHON:-}" ]]; then
    source="$("$COCOTB_PYTHON" -m cocotb_tools.config --makefiles)"
else
    source="$(cocotb-config --makefiles)"
fi

mkdir -p "$target/simulators"
for file in "$source"/*; do
    [[ "$(basename "$file")" == simulators ]] && continue
    ln -sfn "$file" "$target/$(basename "$file")"
done
for file in "$source"/simulators/*; do
    ln -sfn "$file" "$target/simulators/$(basename "$file")"
done
if [[ -e "$source/simulators/Makefile.enzan" ]]; then
    printf '%s: this cocotb supports Enzan itself, so the copy keeps its Makefile.enzan; %s\n' \
        "$(basename "$0")" '`make SIM=enzan` works without COCOTB_MAKEFILES_DIR' >&2
else
    ln -sfn "$here/Makefile.enzan" "$target/simulators/Makefile.enzan"
fi
