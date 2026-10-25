#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Command lines that are wrong, and what Enzan says of them (DESIGN_GUIDE.md section 8). A
# command line is wrong when it asks for nothing, for two things at once, or for something the
# command it gives does not do; each is refused with status 1 and a message saying which, rather
# than run as if the option had been taken. An option that changes nothing and says nothing is
# indistinguishable from one that worked.
#
#   run-cli-check.sh <enzan>
#
# Every build runs it: a command line is checked before any frontend is reached. What needs a
# design to analyse runs in the languages the build has, which `enzan --version` lists.

set -uo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <enzan>" >&2
    exit 2
fi

enzan=$1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 2

fail() {
    echo "FAIL cli: $1" >&2
    exit 1
}

# Runs a command, holding it to the status it must end with and to a part of what it must say.
expect() {
    local status=$1 said=$2
    shift 2
    local output actual=0
    output=$("$@" 2>&1) || actual=$?
    [[ $actual -eq $status ]] || fail "'$*' ended with $actual rather than $status: $output"
    grep -qF -- "$said" <<<"$output" || fail "'$*' did not say '$said': $output"
}

components=$("$enzan" --version | sed -n 's/^Optional components: //p')
has() {
    [[ ", $components, " == *", $1, "* ]]
}

# One top level in each language the build has, and the source to run it from.
sources=()
if has systemverilog; then
    printf 'module tb;\n  initial $display("PASS cli tb");\nendmodule\n' >tb.sv
    sources+=(tb.sv)
fi
if has vhdl; then
    printf 'entity tb is end entity;\narchitecture a of tb is\nbegin\n' >tb.vhd
    printf '  process begin report "PASS cli tb"; wait; end process;\nend architecture;\n' >>tb.vhd
    [[ ${#sources[@]} -eq 0 ]] && sources+=(tb.vhd)
fi

# --- the shape of the command (8.1) ---------------------------------------------------------------

# `lib` is a command whatever follows it, rather than a source to read.
expect 1 "\`lib\` takes create, list or clean" "$enzan" lib
expect 1 "\`lib\` takes create, list or clean" "$enzan" lib frob libs/x
[[ $("$enzan" lib 2>&1) != *"cannot open"* ]] || fail "a lone \`lib\` was read as a source as well"

# `run` and `elaborate` elaborate a design from libraries or a file list, and with neither have
# none; that is the command line's mistake, not the design's.
expect 1 "needs a design" "$enzan" run
expect 1 "needs a design" "$enzan" run tb
expect 1 "needs a design" "$enzan" elaborate tb

# A source where the top level goes is a slip of the one-shot form, and said to be one rather
# than looked for as a top.
printf 'module tb; endmodule\n' >slip.sv
expect 1 "'slip.sv' is a source" "$enzan" run --in-lib libs/none slip.sv
expect 1 "'slip.sv' is a source" "$enzan" elaborate --in-lib libs/none slip.sv

# --- a dependency file (8.3) ---------------------------------------------------------------------

# Its rule needs a target, and a target needs the file to be written; a rule for no target is one
# make refuses to read.
expect 1 "'--depfile-target'" "$enzan" --depfile-target stamp tb.sv
expect 1 "'--depfile' needs a target" "$enzan" analyse --depfile deps.d tb.sv
expect 1 "'--depfile'" "$enzan" run --in-lib libs/none --depfile deps.d --depfile-target stamp tb

# Written wherever sources are analysed, and not only by `analyse`.
if [[ ${#sources[@]} -gt 0 ]]; then
    "$enzan" --depfile deps.d --depfile-target stamp "${sources[0]}" >/dev/null 2>&1 \
        || fail "a one-shot run writing a dependency file failed"
    grep -q "^stamp:" deps.d && grep -qF "${sources[0]}" deps.d \
        || fail "a one-shot run did not write its dependency file: $(cat deps.d 2>&1)"
fi

echo "PASS cli"
