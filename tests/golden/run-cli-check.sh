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
golden_dir=$(cd "$(dirname "$0")" && pwd)

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
expect 1 "option '--backend' needs a value" "$enzan" --backend
expect 1 "takes interpreter or jit" "$enzan" --backend unknown tb.sv
expect 1 "'--backend' was given twice" "$enzan" --backend interpreter --backend jit tb.sv
expect 1 "'--backend' applies" "$enzan" analyse --backend interpreter tb.sv
expect 1 "'--backend' needs a simulation" "$enzan" --backend interpreter --dry-run tb.sv
expect 1 "cannot select the backend of a running simulation" \
    "$enzan" --gui --attach k.sock --backend interpreter
if ! has jit; then
    expect 1 "JIT backend is not available" "$enzan" --backend jit tb.sv
fi

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
expect 1 "'--phase-timings' applies" "$enzan" analyse --phase-timings tb.sv
expect 1 "cannot measure a simulation already running" "$enzan" --gui --attach k.sock --phase-timings
expect 1 "'--phase-timings' needs a simulation" "$enzan" --phase-timings --dry-run tb.sv
expect 1 "'--phase-timings' needs a simulation" "$enzan" lib list --phase-timings libs/none
expect 1 "'--activity-stats' applies" "$enzan" analyse --activity-stats tb.sv
expect 1 "cannot measure a simulation already running" "$enzan" --gui --attach k.sock --activity-stats
expect 1 "'--activity-stats' needs a simulation" "$enzan" --activity-stats --dry-run tb.sv
expect 1 "'--activity-stats' needs a simulation" "$enzan" lib list --activity-stats libs/none

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
    expect 0 "PASS cli tb" "$enzan" --backend interpreter "${sources[0]}"
    "$enzan" --phase-timings "${sources[0]}" >timing-out.txt 2>timing-err.txt \
        || fail "a run with phase timings failed"
    grep -q 'PASS cli tb' timing-out.txt timing-err.txt \
        || fail "timing changed the design transcript"
    grep -Eq '^-- phase timings \(ns\): analysis=[0-9]+ elaboration=[0-9]+ bytecode=[0-9]+ image=[0-9]+ simulation=[0-9]+$' timing-err.txt \
        || fail "phase timing fields are missing or malformed"
    "$enzan" "${sources[0]}" >untimed-out.txt 2>untimed-err.txt \
        || fail "a run without phase timings failed"
    cmp -s timing-out.txt untimed-out.txt || fail "timing changed the transcript"
    ! grep -q 'phase timings' untimed-err.txt || fail "timing is not opt-in"
fi
if has systemverilog && has jit; then
    printf 'module tb; initial begin $display("JIT starts"); #1 $finish; end endmodule\n' >jit.sv
    "$enzan" --backend jit --phase-timings jit.sv >jit-out.txt 2>jit-err.txt \
        || fail "a JIT run failed"
    grep -q 'JIT starts' jit-out.txt || fail "JIT did not run the process"
    grep -Eq '^-- JIT preparation \(ns\): [0-9]+$' jit-err.txt \
        || fail "JIT preparation time is missing"
    printf 'module tb; integer a=2,b=3,c; initial begin $display("must not run"); c=a**b; end endmodule\n' \
        >jit-unsupported.sv
    expect 2 "JIT preparation failed" "$enzan" --backend jit jit-unsupported.sv
    ! "$enzan" --backend jit jit-unsupported.sv >jit-refused.txt 2>&1 \
        || fail "an unsupported JIT operation ran"
    ! grep -Fxq 'must not run' jit-refused.txt || fail "JIT failed after simulation began"

    cat >jit-events.sv <<'EOF'
`timescale 1ns/1ns
module tb;
  reg clock = 0;
  reg value = 0;
  initial begin #1 clock = 1'b1; #1 $display("event value=%b", value); $finish; end
  initial begin @(posedge clock); value <= 1'b1; end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-events.sv >jit-events-native.txt 2>&1 \
        || fail "JIT event test failed"
    "$enzan" --backend interpreter --dump-stats jit-events.sv >jit-events-interpreter.txt 2>&1 \
        || fail "interpreter event test failed"
    cmp -s jit-events-native.txt jit-events-interpreter.txt \
        || fail "JIT event scheduling or work counts differ from the interpreter"
    grep -q 'event value=1' jit-events-native.txt || fail "nonblocking update did not run"

    cat >jit-bits.sv <<'EOF'
module tb;
  reg [3:0] value = 4'b0x1z;
  reg [3:0] result;
  initial begin result = ~value; $display("bits=%b", result); $finish; end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-bits.sv >jit-bits-native.txt 2>&1 \
        || fail "JIT bitwise test failed"
    "$enzan" --backend interpreter --dump-stats jit-bits.sv >jit-bits-interpreter.txt 2>&1 \
        || fail "interpreter bitwise test failed"
    cmp -s jit-bits-native.txt jit-bits-interpreter.txt \
        || fail "JIT bitwise result or work counts differ from the interpreter"

    cat >jit-counter.sv <<'EOF'
`timescale 1ns/1ns
module tb;
  reg [3:0] count = 4'd0;
  initial begin
    while (count < 4'd4) begin #1 count = count + 4'd1; end
    $display("counter=%0d", count);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-counter.sv >jit-counter-native.txt 2>&1 \
        || fail "JIT counter test failed"
    "$enzan" --backend interpreter --dump-stats jit-counter.sv >jit-counter-interpreter.txt 2>&1 \
        || fail "interpreter counter test failed"
    cmp -s jit-counter-native.txt jit-counter-interpreter.txt \
        || fail "JIT counter result or work counts differ from the interpreter"
    grep -q '^counter=4$' jit-counter-native.txt || fail "counter did not reach four"

    cat >jit-call.sv <<'EOF'
module tb;
  function automatic [7:0] twice(input [7:0] value);
    twice = value + value;
  endfunction
  reg [7:0] result;
  initial begin
    result = twice(8'd21);
    $display("call=%d", result);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-call.sv >jit-call-native.txt 2>&1 \
        || fail "JIT function call failed"
    "$enzan" --backend interpreter --dump-stats jit-call.sv >jit-call-interpreter.txt 2>&1 \
        || fail "interpreter function call failed"
    cmp -s jit-call-native.txt jit-call-interpreter.txt \
        || fail "JIT function result or work counts differ from the interpreter"
    grep -q '^call= *42$' jit-call-native.txt || fail "function did not return 42"

    cat >jit-real.sv <<'EOF'
module tb;
  real a = 1.5, b = 2.0;
  initial begin
    if (a < b) $display("real comparison passed");
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-real.sv >jit-real-native.txt 2>&1 \
        || fail "JIT real comparison failed"
    "$enzan" --backend interpreter --dump-stats jit-real.sv >jit-real-interpreter.txt 2>&1 \
        || fail "interpreter real comparison failed"
    cmp -s jit-real-native.txt jit-real-interpreter.txt \
        || fail "JIT real comparison or work counts differ from the interpreter"
    grep -q '^real comparison passed$' jit-real-native.txt || fail "real comparison failed"

    cat >jit-select.sv <<'EOF'
module tb;
  reg [7:0] value = 8'b1x10z011;
  reg [3:0] selected;
  integer index = 3;
  initial begin
    selected = value[index +: 4];
    $display("selected=%b", selected);
    value[index +: 4] = 4'bz10x;
    $display("value=%b", value);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-select.sv >jit-select-native.txt 2>&1 \
        || fail "JIT partial object access failed"
    "$enzan" --backend interpreter --dump-stats jit-select.sv >jit-select-interpreter.txt 2>&1 \
        || fail "interpreter partial object access failed"
    cmp -s jit-select-native.txt jit-select-interpreter.txt \
        || fail "JIT partial object access or work counts differ from the interpreter"
    grep -q '^selected=x10z$' jit-select-native.txt || fail "slice read gave the wrong value"
    grep -q '^value=1z10x011$' jit-select-native.txt || fail "slice write gave the wrong value"

    cat >jit-div.sv <<'EOF'
module tb;
  integer a = -20, b = 3;
  initial begin
    $display("division=%0d remainder=%0d", a / b, a % b);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-div.sv >jit-div-native.txt 2>&1 \
        || fail "JIT division failed"
    "$enzan" --backend interpreter --dump-stats jit-div.sv >jit-div-interpreter.txt 2>&1 \
        || fail "interpreter division failed"
    cmp -s jit-div-native.txt jit-div-interpreter.txt \
        || fail "JIT division or work counts differ from the interpreter"
    grep -q '^division=-6 remainder=-2$' jit-div-native.txt || fail "signed division failed"

    cat >jit-cond.sv <<'EOF'
module tb;
  reg [3:0] a = 4'b10xz, b = 4'b1zxx, value;
  reg condition = 1'bx;
  initial begin
    value = condition ? a : b;
    $display("conditional=%b", value);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-cond.sv >jit-cond-native.txt 2>&1 \
        || fail "JIT conditional failed"
    "$enzan" --backend interpreter --dump-stats jit-cond.sv >jit-cond-interpreter.txt 2>&1 \
        || fail "interpreter conditional failed"
    cmp -s jit-cond-native.txt jit-cond-interpreter.txt \
        || fail "JIT conditional or work counts differ from the interpreter"
    grep -q '^conditional=1xxx$' jit-cond-native.txt || fail "conditional merge failed"

    cat >jit-random.sv <<'EOF'
module tb;
  reg [31:0] value;
  initial begin
    value = $urandom;
    $display("random=%h", value);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-random.sv >jit-random-native.txt 2>&1 \
        || fail "JIT system function failed"
    "$enzan" --backend interpreter --dump-stats jit-random.sv >jit-random-interpreter.txt 2>&1 \
        || fail "interpreter system function failed"
    cmp -s jit-random-native.txt jit-random-interpreter.txt \
        || fail "JIT system function or work counts differ from the interpreter"
    grep -q '^random=' jit-random-native.txt || fail "system function did not run"

    cat >jit-arith.sv <<'EOF'
module tb;
  real a = 1.5, b = 2.0, value;
  initial begin
    value = (a + b) * b;
    $display("real arithmetic=%0.1f", value);
    $finish;
  end
endmodule
EOF
    "$enzan" --backend jit --dump-stats jit-arith.sv >jit-arith-native.txt 2>&1 \
        || fail "JIT real arithmetic failed"
    "$enzan" --backend interpreter --dump-stats jit-arith.sv >jit-arith-interpreter.txt 2>&1 \
        || fail "interpreter real arithmetic failed"
    cmp -s jit-arith-native.txt jit-arith-interpreter.txt \
        || fail "JIT real arithmetic or work counts differ from the interpreter"
    grep -Fxq 'real arithmetic=7.0' jit-arith-native.txt || fail "real arithmetic failed"

    pipeline="$golden_dir/../../bench/designs/pipeline.sv"
    "$enzan" --backend jit --top bench -G CYCLES=20 --timescale ns --dump-stats "$pipeline" \
        >jit-pipeline-native.txt 2>&1 || fail "JIT pipeline test failed"
    "$enzan" --backend interpreter --top bench -G CYCLES=20 --timescale ns --dump-stats "$pipeline" \
        >jit-pipeline-interpreter.txt 2>&1 || fail "interpreter pipeline test failed"
    cmp -s jit-pipeline-native.txt jit-pipeline-interpreter.txt \
        || fail "JIT pipeline result or work counts differ from the interpreter"
    grep -q '^cycles=20 ' jit-pipeline-native.txt || fail "pipeline did not reach 20 cycles"
fi
if has systemverilog; then
    cat >activity.sv <<'EOF'
`timescale 1ns/1ns
module activity;
  reg q = 0;
  initial begin
    #1 q <= 1;
    #1 q <= 1;
    #1 $display("activity q=%b", q);
    $finish;
  end
endmodule
EOF
    "$enzan" --activity-stats activity.sv >activity-out.txt 2>activity-err.txt \
        || fail "a run with activity counters failed"
    "$enzan" activity.sv >activity-plain-out.txt 2>activity-plain-err.txt \
        || fail "a run without activity counters failed"
    cmp -s activity-out.txt activity-plain-out.txt || fail "activity counters changed the transcript"
    ! grep -q '^-- activity:' activity-plain-err.txt || fail "activity counters are not opt-in"
    grep -Fxq -- '-- activity: process_runs=4 connection_runs=0 enqueues=1 signal_changes=1 nba_updates=2 transactions=0 resolution_calls=0' activity-err.txt \
        || fail "activity counters did not reflect the known process and update sequence"

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
if has vhdl && has jit; then
    cat >jit-clock.vhd <<'EOF'
entity clock is end entity;
architecture a of clock is
  signal c : bit;
begin
  c <= not c after 5 ns;
end architecture;
EOF
    "$enzan" --backend jit --dump-stats --activity-stats --run 20ns jit-clock.vhd \
        >jit-clock-native.txt 2>&1 || fail "JIT VHDL clock failed"
    "$enzan" --backend interpreter --dump-stats --activity-stats --run 20ns jit-clock.vhd \
        >jit-clock-interpreter.txt 2>&1 || fail "interpreter VHDL clock failed"
    cmp -s jit-clock-native.txt jit-clock-interpreter.txt \
        || fail "JIT VHDL transactions or work counts differ from the interpreter"
    grep -Eq ' transactions=[1-9][0-9]* ' jit-clock-native.txt \
        || fail "VHDL clock did not apply driver transactions"

    cat >jit-attribute.vhd <<'EOF'
entity tb is end entity;
architecture a of tb is
  signal clk : bit := '0';
begin
  clk <= '1' after 5 ns;
  process(clk)
  begin
    if clk'event then report "event observed"; end if;
  end process;
end architecture;
EOF
    "$enzan" --backend jit --dump-stats --run 10ns jit-attribute.vhd \
        >jit-attribute-native.txt 2>&1 || fail "JIT VHDL event attribute failed"
    "$enzan" --backend interpreter --dump-stats --run 10ns jit-attribute.vhd \
        >jit-attribute-interpreter.txt 2>&1 || fail "interpreter VHDL event attribute failed"
    cmp -s jit-attribute-native.txt jit-attribute-interpreter.txt \
        || fail "JIT VHDL event attribute or work counts differ from the interpreter"
    grep -q 'event observed' jit-attribute-native.txt || fail "VHDL event was not observed"

    cat >jit-partial.vhd <<'EOF'
entity tb is end entity;
architecture a of tb is
  signal data : bit_vector(7 downto 0) := (others => '0');
begin
  data(3 downto 0) <= "1010" after 5 ns;
  process
  begin
    wait for 6 ns;
    assert data = "00001010" report "incorrect slice" severity failure;
    report "slice passed";
    wait;
  end process;
end architecture;
EOF
    "$enzan" --backend jit --dump-stats --run 10ns jit-partial.vhd \
        >jit-partial-native.txt 2>&1 || fail "JIT VHDL slice failed"
    "$enzan" --backend interpreter --dump-stats --run 10ns jit-partial.vhd \
        >jit-partial-interpreter.txt 2>&1 || fail "interpreter VHDL slice failed"
    cmp -s jit-partial-native.txt jit-partial-interpreter.txt \
        || fail "JIT VHDL slice or work counts differ from the interpreter"
    grep -q 'slice passed' jit-partial-native.txt || fail "VHDL slice was incorrect"
fi
if has vhdl; then
    "$enzan" --activity-stats --top tb "$golden_dir/vhdl/ports.vhd" \
        >connection-out.txt 2>connection-err.txt || fail "an instrumented port connection run failed"
    grep -Eq '^-- activity: process_runs=[0-9]+ connection_runs=[1-9][0-9]* ' connection-err.txt \
        || fail "activity counters missed port connection activations"
    "$enzan" --activity-stats --top tb "$golden_dir/vhdl/local_resolution.vhd" \
        >resolution-out.txt 2>resolution-err.txt || fail "an instrumented VHDL resolution run failed"
    grep -Eq ' transactions=[1-9][0-9]* resolution_calls=[1-9][0-9]*$' resolution-err.txt \
        || fail "activity counters missed transactions or resolution calls"
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
