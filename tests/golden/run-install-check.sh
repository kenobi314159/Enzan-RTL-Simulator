#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# An installation of this build, moved and made read-only, runs as the build does (DESIGN_GUIDE.md
# section 11.5). It installs nothing that is not Enzan's: a dependency built in the tree may bring
# install rules of its own, and those would put a library's headers into every package.
#
#   run-install-check.sh <cmake> <build directory> <gui ON|OFF> <bin> <libexec> <vhdl> <include> <data>
#
# The directories are where the install rules put each part, relative to the prefix. What needs a
# design to run runs in the languages the build has, which `enzan --version` lists.

set -uo pipefail

if [[ $# -ne 8 ]]; then
    echo "usage: $0 <cmake> <build directory> <gui ON|OFF> <bin> <libexec> <vhdl> <include> <data>" >&2
    exit 2
fi

cmake=$1 build=$2 gui=$3 bin=$4 libexec=$5 vhdl=$6 include=$7 data=$8

work=$(mktemp -d)
# Writable again first, or the tree made read-only below could not be removed.
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT

fail() {
    echo "FAIL install: $1" >&2
    exit 1
}

"$cmake" --install "$build" --prefix "$work/installed" >"$work/install.log" 2>&1 \
    || fail "cmake --install failed: $(cat "$work/install.log")"

# --- what is installed ---------------------------------------------------------------------------

while IFS= read -r file; do
    case $file in
        "$bin/enzan" | "$include/vpi_user.h" | "$include/sv_vpi_user.h" | "$include/vhpi_user.h") ;;
        "$data/cocotb/Makefile.enzan" | "$data/cocotb/makefiles.sh" | "$vhdl"/*) ;;
        "$libexec/enzan-gui") [[ $gui == ON ]] || fail "a build without the GUI installs $file" ;;
        *) fail "installs $file, which is not part of Enzan" ;;
    esac
done < <(cd "$work/installed" && find . \( -type f -o -type l \) | sed 's|^\./||' | sort)

[[ -x $work/installed/$bin/enzan ]] || fail "installs no $bin/enzan"

components=$("$work/installed/$bin/enzan" --version | sed -n 's/^Optional components: //p')
has() {
    [[ ", $components, " == *", $1, "* ]]
}

if [[ $gui == ON ]]; then
    [[ -x $work/installed/$libexec/enzan-gui ]] || fail "installs no $libexec/enzan-gui"
fi
if has vhdl; then
    [[ -f $work/installed/$vhdl/ieee/_NVC_LIB ]] || fail "installs no VHDL standard libraries"
    [[ -f $work/installed/$include/vhpi_user.h ]] || fail "installs no vhpi_user.h"
fi
if has systemverilog || has vhdl; then
    [[ -f $work/installed/$include/vpi_user.h ]] || fail "installs no vpi_user.h"
    [[ -x $work/installed/$data/cocotb/makefiles.sh ]] || fail "installs no cocotb makefiles"
fi

# --- what it does, somewhere else, unwritable ----------------------------------------------------

# Nothing in the installation may be written once it is installed: its user is usually not the one
# who installed it. Root writes regardless, so where the suite runs as root this proves less.
mv "$work/installed" "$work/moved"
chmod -R a-w "$work/moved"
enzan=$work/moved/$bin/enzan
mkdir "$work/run"
cd "$work/run" || exit 2

"$enzan" --version >/dev/null || fail "the moved installation's enzan --version failed"

run() {
    local said=$1 output
    shift
    output=$("$enzan" "$@" 2>&1) || fail "'enzan $*' failed: $output"
    grep -qF -- "$said" <<<"$output" || fail "'enzan $*' did not say '$said': $output"
}

if has systemverilog; then
    printf 'module tb;\n  initial $display("PASS install sv");\nendmodule\n' >tb.sv
    run "PASS install sv" --top tb tb.sv
fi

# The standard libraries are what a VHDL design finds without naming them (4.4), so the design uses
# one of each library a run reads them from: std, and ieee's 2008 numeric_std.
if has vhdl; then
    {
        printf 'library ieee;\nuse ieee.numeric_std.all;\nuse std.textio.all;\n'
        printf 'entity tb is end entity;\narchitecture a of tb is\nbegin\n  process\n'
        printf '    variable l : line;\n  begin\n'
        printf '    write(l, string'"'"'("PASS install vhdl ") & to_string(to_unsigned(5, 4)));\n'
        printf '    writeline(output, l);\n    wait;\n  end process;\nend architecture;\n'
    } >tb.vhd
    run "PASS install vhdl 0101" --top tb tb.vhd
fi

# The front end `enzan --gui` starts: golden.cli shows `enzan` starts what is at this path and
# unit.common.Installation that the path is this one, so here it is enough that it runs from it.
if [[ $gui == ON ]]; then
    QT_QPA_PLATFORM=offscreen "$work/moved/$libexec/enzan-gui" --version >/dev/null 2>&1 \
        || fail "the moved installation's enzan-gui --version failed"
fi

echo "PASS install"
