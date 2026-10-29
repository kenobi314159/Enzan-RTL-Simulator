#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs an installed package of Enzan as a user would (DESIGN_GUIDE.md section 11.7):
#
#   smoke-test.sh kernel    with the enzan package installed and enzan-gui not
#   smoke-test.sh gui       with both installed
#
# The packages' own build ran the test suite; this is what that cannot show, that what was
# installed works from where it was installed, run as `enzan` from the PATH.

set -uo pipefail

fail() {
    echo "FAIL smoke-test: $1" >&2
    exit 1
}

[[ $# -eq 1 && ($1 == kernel || $1 == gui) ]] || fail "usage: $0 kernel|gui"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 1

enzan --version || fail "enzan --version failed"

# The command's status, while its output goes to out.log.
status_of() {
    "$@" >out.log 2>&1
    echo $?
}

if [[ $1 == kernel ]]; then
    printf 'module tb;\n  initial $display("PASS smoke sv");\nendmodule\n' >tb.sv
    output=$(enzan --top tb tb.sv 2>&1) || fail "the SystemVerilog design failed: $output"
    grep -qF "PASS smoke sv" <<<"$output" || fail "the SystemVerilog design said: $output"

    # VHDL's standard libraries are found from where `enzan` is, without being named (4.4).
    {
        printf 'library ieee;\nuse ieee.numeric_std.all;\nuse std.textio.all;\n'
        printf 'entity tb is end entity;\narchitecture a of tb is\nbegin\n  process\n'
        printf '    variable l : line;\n  begin\n'
        printf '    write(l, string'"'"'("PASS smoke vhdl ") & to_string(to_unsigned(5, 4)));\n'
        printf '    writeline(output, l);\n    wait;\n  end process;\nend architecture;\n'
    } >tb.vhd
    output=$(enzan --top tb tb.vhd 2>&1) || fail "the VHDL design failed: $output"
    grep -qF "PASS smoke vhdl 0101" <<<"$output" || fail "the VHDL design said: $output"

    for file in /usr/include/enzan/vpi_user.h /usr/include/enzan/sv_vpi_user.h \
        /usr/include/enzan/vhpi_user.h /usr/share/enzan/cocotb/Makefile.enzan; do
        [[ -f $file ]] || fail "$file is not installed"
    done
    [[ -x /usr/share/enzan/cocotb/makefiles.sh ]] || fail "makefiles.sh is not installed"

    # Without its front end, `enzan --gui` says where the front end would be found.
    status=$(status_of enzan --gui --attach "$work/no-kernel")
    [[ $status -eq 1 ]] || fail "enzan --gui without enzan-gui ended with $status: $(cat out.log)"
    grep -qF "it is not installed" out.log || fail "enzan --gui without enzan-gui said: $(cat out.log)"
else
    # The window `enzan --gui` starts stays open when it finds no kernel, saying so: still running
    # when the time is up is how this shows that `enzan` found it and Qt brought it up.
    status=$(status_of env QT_QPA_PLATFORM=offscreen timeout 10 enzan --gui --attach "$work/no-kernel")
    [[ $status -eq 124 ]] || fail "enzan --gui ended with $status rather than staying open: $(cat out.log)"
fi

echo "PASS smoke-test $1"
