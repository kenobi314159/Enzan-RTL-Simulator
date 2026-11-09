#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Command lines that are wrong, and what Enzan says of them. A command line is wrong when it asks
# for nothing, for two things at once, or for something the command it gives does not do; each is
# refused with status 1 and a message saying which, rather than run as if the option had been
# taken. An option that changes nothing and says nothing is indistinguishable from one that worked.
#
#   run-cli-check.sh <enzan> <front end>
#
# <front end> is where `enzan` looks for the graphical front end, relative to its own directory.
# Every build runs it: a command line is checked before any frontend is reached. What needs a design
# to analyse runs in the languages the build has, which `enzan --version` lists.

set -uo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <enzan> <front end>" >&2
    exit 2
fi

enzan=$1
front_end=$2

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

# --- what every invocation can count on -----------------------------------------------------------

[[ $("$enzan" --version | head -1) =~ ^enzan\ [0-9]+\.[0-9]+\.[0-9]+ ]] \
    || fail "--version does not begin with the version: $("$enzan" --version 2>&1 | head -1)"
expect 0 "Usage: enzan" "$enzan" --help
expect 0 "--listen <socket>" "$enzan" --help
expect 1 "unrecognised option '--no-such-option'" "$enzan" --no-such-option
expect 1 "Usage: enzan" "$enzan"
expect 1 "no source files were given" "$enzan" --top tb
expect 1 "option '--top' needs a value" "$enzan" --top

# A served session is driven by its client, and a signal list fills a window a headless run does
# not have: each refused rather than dropped.
expect 1 "cannot be combined" "$enzan" --listen k.sock --run 10 tb.sv
expect 1 "cannot be combined" "$enzan" --listen k.sock --run all tb.sv
expect 1 "'--wave-list' needs '--gui'" "$enzan" run --wave-list signals.tcl tb

# --- the shape of the command ---------------------------------------------------------------------

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

# --- a simulation's options where nothing is simulated --------------------------------------------

# A waveform `analyse` never writes, a run length `elaborate` never runs: refused, as `--vpi` is,
# rather than taken and dropped.
expect 1 "'--vcd' applies to a simulation, and \`analyse\` runs none" "$enzan" analyse --vcd a.vcd tb.sv
expect 1 "'--run' and '--seed' apply" "$enzan" analyse --run 10 --seed 3 tb.sv
expect 1 "\`elaborate\` runs none" "$enzan" elaborate --in-lib libs/none --fst a.fst tb
expect 1 "'--gui' applies" "$enzan" elaborate --in-lib libs/none --gui tb
expect 1 "'--listen' applies" "$enzan" analyse --listen k.sock tb.sv

# --- an option given twice ------------------------------------------------------------------------

# Two values for one are two answers to one question, which the last no longer settles; the same
# value again is the same answer. `--dry-run` stops before anything is analysed.
expect 1 "'--vcd' was given twice" "$enzan" --vcd a.vcd --vcd b.vcd tb.sv
expect 1 "'--wave-scope' was given twice" "$enzan" --vcd-scope /tb --wave-scope /tb/u tb.sv
expect 1 "sets 'W' twice" "$enzan" -G W=1 -GW=2 tb.sv
expect 0 "tb.sv" "$enzan" --dry-run -t tb --top tb -G W=1 -GW=1 tb.sv

# --- several top levels --------------------------------------------------------------------------

# `--top` is repeatable, each a root and the first the design's own; the same top again is the
# same one. With none named, two candidates are refused, and `glbl` among them is said to be a top
# of its own.
if has systemverilog; then
    printf 'module tb;\n  initial $display("PASS cli tb");\nendmodule\nmodule glbl;\nendmodule\n' \
        >tops.sv
    expect 0 "PASS cli tb" "$enzan" --top tb --top glbl --top tb tops.sv
    expect 2 "top module 'other' was not found" "$enzan" --top tb --top other tb.sv
    expect 2 "give \`--top\` for each" "$enzan" tops.sv
fi

# --- how long a run runs --------------------------------------------------------------------------

# A count of the run's time unit, or a time with a unit of its own, which has to be a whole number
# of the run's unit once the design has settled what that is; or `all`, until the design finishes,
# as no `--run` does.
expect 1 "'1.5' is not a length of time" "$enzan" --run 1.5 tb.sv
expect 1 "'10xs' is not a length of time" "$enzan" --run 10xs tb.sv
expect 1 "'ALL' is not a length of time" "$enzan" --run ALL tb.sv
if [[ ${#sources[@]} -gt 0 ]]; then
    expect 0 "PASS cli tb" "$enzan" --run all "${sources[0]}"
fi
if has systemverilog; then
    printf '`timescale 1ns/1ps\nmodule clock;\n  reg c = 0;\n  always #5 c = ~c;\nendmodule\n' \
        >clock.sv
    expect 0 "ended at 1500000ps" "$enzan" --dump-stats --run 1.5us clock.sv
    expect 0 "ended at 100000ps" "$enzan" --dump-stats --run 100000 clock.sv
    expect 1 "'10fs' is not a whole number of the run's time unit, ps" \
        "$enzan" --run 10fs clock.sv
elif has vhdl; then
    printf 'entity clock is end entity;\narchitecture a of clock is\n  signal c : bit;\n' >clock.vhd
    printf 'begin\n  c <= not c after 5 ns;\nend architecture;\n' >>clock.vhd
    expect 0 "ended at 1500000000fs" "$enzan" --dump-stats --run 1.5us clock.vhd
fi

# --- what reaches the window ----------------------------------------------------------------------

# The window's own options are forwarded as they were written, and none it was not given: the window
# reads `--run` again, by the same function, once the kernel has said what unit it counts. `enzan`
# starts the front end it finds from its own directory, so a copy of it is put where that leads to a
# stand-in that says what it was given, with the build's lib/ beside the copy's bin/ for VHDL's
# standard libraries; `--attach` starts no kernel, so this needs no design and no display.
mkdir -p window/bin
cp "$enzan" window/bin/enzan
ln -s "$(cd "$(dirname "$enzan")/.." && pwd)/lib" window/lib
stand_in_path=window/bin/$front_end
mkdir -p "$(dirname "$stand_in_path")"
printf '#!/bin/sh\necho "front end: $*"\n' >"$stand_in_path"
chmod +x "$stand_in_path"
expect 0 "front end: --attach k.sock" window/bin/enzan --gui --attach k.sock
expect 0 "front end: --run 10ns --attach k.sock" window/bin/enzan --gui --attach k.sock --run 10ns
expect 0 "front end: --run all --attach k.sock" window/bin/enzan --gui --attach k.sock -r all

# A window `--gui` started that failed is not one its reader closed: it is said to have, and the
# status is 4, where a closed one leaves the run's. One that cannot be started at all is said to be
# at once. SIGTERM rather than a crash's signal, so that no core is dumped.
if [[ ${#sources[@]} -gt 0 ]]; then
    stand_in() {
        printf '#!/bin/sh\n%s\n' "$1" >"$stand_in_path"
        chmod +x "$stand_in_path"
    }
    stand_in 'exit 0'
    expect 0 "" window/bin/enzan --gui "${sources[0]}"
    stand_in 'kill -TERM $$'
    expect 4 "the graphical front end ended on signal 15" window/bin/enzan --gui "${sources[0]}"
    stand_in 'exit 7'
    expect 4 "the graphical front end exited with status 7" window/bin/enzan --gui "${sources[0]}"
    # Missing, and not looked for anywhere else: a front end on the PATH could be another
    # version's.
    rm "$stand_in_path"
    mkdir -p elsewhere
    printf '#!/bin/sh\necho "a front end on the PATH"\n' >elsewhere/enzan-gui
    chmod +x elsewhere/enzan-gui
    expect 1 "could not start the graphical front end" \
        env PATH="$work/elsewhere:$PATH" window/bin/enzan --gui "${sources[0]}"
fi

# --- a dependency file ----------------------------------------------------------------------------

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
