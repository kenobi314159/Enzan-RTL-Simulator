#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The command line of DESIGN_GUIDE.md section 8, driven the way a Makefile drives it: separate
# processes for analysis and for simulation, with a library between them. That separation is
# the point — a library exists so that what one process analysed another can run — so it cannot
# be checked from inside one invocation.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <enzan> <source.sv>" >&2
    exit 2
fi

enzan=$1
source_file=$2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$source_file" "$work/counter.sv"

fail() {
    echo "FAIL library: $1" >&2
    exit 1
}

# Runs a command and returns its status without `set -e` ending the script first, which is
# what every check for a *refusal* below needs.
status_of() {
    local status=0
    "$@" >/dev/null 2>&1 || status=$?
    echo "$status"
}

cd "$work"

# --- a library is created before it is used -----------------------------------------------------

# Opt-in (8.2): a directory that was never created is not a library, so analysing into one that
# does not exist creates it, and reading one that does not exist is an error.
[[ $(status_of "$enzan" run --in-lib absent --timescale ns tb) -ne 0 ]] \
    || fail "reading a library that was never created was allowed"

"$enzan" lib create libs/work >/dev/null || fail "a library could not be created"
[[ -f libs/work/enzan.lib ]] || fail "a created library carries no index"

# --- analysis and simulation are separate processes ----------------------------------------------

printf 'counter.sv\n' >rtl.f
"$enzan" analyse --timescale ns --out-lib libs/work -f rtl.f || fail "analysis into a library failed"

listing=$("$enzan" lib list libs/work)
grep -q "library 'work'" <<<"$listing" || fail "the library does not report its own name"
grep -q 'counter' <<<"$listing" || fail "the library does not hold what was analysed into it"

# The entry is named after its source, so a reader can see what is in the directory.
ls libs/work | grep -q 'counter.sv' || fail "an entry is not named after the source it holds"

run_output=$("$enzan" run --in-lib libs/work --timescale ns tb) \
    || fail "a design analysed by one process did not run in another"
grep -q 'PASS counter' <<<"$run_output" || fail "the run did not reach the design's own check"

# --- what only the kernel reads applies to a run from a library ----------------------------------

# A run from libraries alone analyses nothing, and the seed and the delta limit are the run's,
# not the analysis's: given to `run`, they must take effect there.
cat >random.sv <<'EOF'
module random_tb;
    initial $display("drew %0d", $random);
endmodule
EOF
cat >looping.sv <<'EOF'
module looping_tb;
    logic toggle = 1'b0;
    always @(toggle) toggle <= ~toggle;
    initial #1 toggle = 1'b1;
endmodule
EOF
"$enzan" lib create libs/kernel >/dev/null || fail "libs/kernel could not be created"
"$enzan" analyse --timescale ns --out-lib libs/kernel random.sv looping.sv || fail "libs/kernel could not be analysed"

seed0=$("$enzan" run --in-lib libs/kernel --timescale ns random_tb 2>&1) \
    || fail "the random design did not run from its library"
seed7=$("$enzan" run --in-lib libs/kernel --timescale ns --seed 7 random_tb 2>&1) \
    || fail "the random design did not run from its library with a seed"
[[ $(grep drew <<<"$seed0") != $(grep drew <<<"$seed7") ]] \
    || fail "--seed given to a run from a library changed nothing"

looped=$("$enzan" run --in-lib libs/kernel --timescale ns --delta-limit 5 looping_tb 2>&1 || true)
grep -q 'limit of 5' <<<"$looped" \
    || fail "a run from a library did not use its --delta-limit: $looped"

# A macro, an include directory and a parameter override are the analysis's, so a run that
# analyses nothing refuses them rather than running what the library holds as if taken.
[[ $(status_of "$enzan" run --in-lib libs/kernel --timescale ns -G WIDTH=8 random_tb) -eq 1 ]] \
    || fail "-G given to a run from a library was not refused"
[[ $(status_of "$enzan" run --in-lib libs/kernel --timescale ns -D FAST random_tb) -eq 1 ]] \
    || fail "-D given to a run from a library was not refused"
[[ $(status_of "$enzan" elaborate --in-lib libs/kernel --timescale ns +incdir+inc random_tb) -eq 1 ]] \
    || fail "+incdir+ given to an elaboration from a library was not refused"

# --- a library is simulated in the time unit it was analysed in ------------------------------------

# Its delays are counts of that unit, so a run from it takes the unit from it, and a
# `--timescale` naming another would label every time wrongly. It is refused.
cat >timed.sv <<'EOF'
module timed_tb;
    initial #5 $display("at %0t", $time);
endmodule
EOF
"$enzan" lib create libs/timed_ns >/dev/null || fail "libs/timed_ns could not be created"
"$enzan" lib create libs/timed_us >/dev/null || fail "libs/timed_us could not be created"
"$enzan" analyse --timescale ns --out-lib libs/timed_ns timed.sv || fail "libs/timed_ns analysis"
"$enzan" analyse --timescale us --out-lib libs/timed_us timed.sv || fail "libs/timed_us analysis"

timed=$("$enzan" run --in-lib libs/timed_ns timed_tb) \
    || fail "a run from a library without --timescale did not run"
grep -q 'at 5ns' <<<"$timed" || fail "a run did not take its library's unit: $timed"

conflict=$("$enzan" run --in-lib libs/timed_ns --timescale ps timed_tb 2>&1 || true)
[[ $(status_of "$enzan" run --in-lib libs/timed_ns --timescale ps timed_tb) -eq 2 ]] \
    || fail "a --timescale other than the library's was not refused"
grep -q 'ENZ6028' <<<"$conflict" || fail "the conflicting unit was not reported: $conflict"

mixed=$("$enzan" run --in-lib libs/timed_ns --in-lib libs/timed_us timed_ns.timed_tb 2>&1 || true)
grep -q 'ENZ6027' <<<"$mixed" || fail "libraries in two units were combined: $mixed"

# Analysing into a library takes its unit too, and another one is refused until it is cleaned.
[[ $(status_of "$enzan" analyse --timescale ps --out-lib libs/timed_ns timed.sv) -ne 0 ]] \
    || fail "analysing into a library in another unit was allowed"
"$enzan" lib clean libs/timed_ns >/dev/null || fail "libs/timed_ns could not be cleaned"
"$enzan" analyse --timescale ps --out-lib libs/timed_ns timed.sv \
    || fail "a cleaned library could not be filled in another unit"
grep -q 'at 5ps' <<<"$("$enzan" run --in-lib libs/timed_ns timed_tb)" \
    || fail "the cleaned library did not take its new unit"

# --- the file list is expanded, and can be shown without building --------------------------------

expanded=$("$enzan" analyse --dry-run -f rtl.f) || fail "--dry-run failed"
[[ $expanded == "counter.sv" ]] || fail "--dry-run printed '$expanded'"

# --- exit statuses a Makefile can branch on (8.1) ------------------------------------------------

[[ $(status_of "$enzan" --no-such-option counter.sv) -eq 1 ]] \
    || fail "a bad command line did not exit 1"

[[ $(status_of "$enzan" run --in-lib libs/work nosuchtop) -eq 2 ]] \
    || fail "a design that does not elaborate did not exit 2"

cat >failing.sv <<'EOF'
module failing_tb;
    initial $fatal(1, "deliberate");
endmodule
EOF
[[ $(status_of "$enzan" failing.sv) -eq 3 ]] \
    || fail "a design that ran and failed did not exit 3"

# --- two libraries, and a name defined by both ----------------------------------------------------

mkdir -p split
cat >split/one.sv <<'EOF'
module one_tb;
    initial $display("one");
endmodule
EOF
cat >split/two.sv <<'EOF'
module two_tb;
    initial begin $display("PASS split"); $finish; end
endmodule
EOF

"$enzan" lib create libs/one >/dev/null || fail "libs/one could not be created"
"$enzan" lib create libs/two >/dev/null || fail "libs/two could not be created"
"$enzan" analyse --timescale ns --out-lib libs/one split/one.sv || fail "libs/one could not be analysed"
"$enzan" analyse --timescale ns --out-lib libs/two split/two.sv || fail "libs/two could not be analysed"

both=$("$enzan" run --in-lib libs/one --in-lib libs/two --timescale ns two_tb) \
    || fail "two libraries could not be combined"
grep -q 'PASS split' <<<"$both" || fail "the combined run did not reach the design's check"

# One name in two libraries is reported rather than silently resolved (8.2).
"$enzan" lib create libs/clash >/dev/null
"$enzan" analyse --timescale ns --out-lib libs/clash split/two.sv || fail "libs/clash could not be analysed"

clash=$("$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two_tb 2>&1 || true)
[[ $(status_of "$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two_tb) -ne 0 ]] \
    || fail "a name defined by two libraries was resolved rather than reported"
grep -q 'more than one library' <<<"$clash" \
    || fail "the collision was not reported as one: $clash"

# Qualifying it says which was meant.
"$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two.two_tb >/dev/null \
    || fail "a qualified name did not resolve the collision"

# --- a library is parametrisable by whoever uses it ------------------------------------------------

# The point of section 8.2's libraries: a unit is not baked with the parameters the library
# happened to be analysed with. A generic unit is specialised for whatever an instance asks for;
# one that stays specialised, as a type parameter keeps it, is specialised by the consumer, and
# lands in the consumer's library rather than being duplicated from the producer's.
mkdir -p par
cat >par/counter.sv <<'EOF'
module pcounter #(parameter int Width = 4) (output logic [Width-1:0] q);
    initial q = '1;
endmodule

module ptyped #(parameter type T = logic [3:0]) (output T q);
    initial q = '0;
endmodule
EOF
cat >par/wrap.sv <<'EOF'
module pwrap_tb;
    logic [3:0] narrow;
    logic [7:0] wide;
    logic [3:0] typed_narrow;
    logic [7:0] typed_wide;
    pcounter #(.Width(4)) u_narrow(.q(narrow));
    pcounter #(.Width(8)) u_wide(.q(wide));
    ptyped u_typed_narrow(.q(typed_narrow));
    ptyped #(.T(logic [7:0])) u_typed_wide(.q(typed_wide));
    initial begin
        #1;
        if (narrow !== 4'hF || wide !== 8'hFF || typed_narrow !== 4'h0 || typed_wide !== 8'h0) begin
            $display("FAIL library: narrow=%0h wide=%0h", narrow, wide);
            $fatal(1);
        end
        $display("parametrisable ok");
        $finish;
    end
endmodule
EOF

"$enzan" lib create libs/pc >/dev/null || fail "libs/pc could not be created"
"$enzan" lib create libs/pw >/dev/null || fail "libs/pw could not be created"
"$enzan" analyse --timescale ns --out-lib libs/pc par/counter.sv || fail "the counter library could not be analysed"
"$enzan" analyse --in-lib libs/pc --out-lib libs/pw par/wrap.sv \
    || fail "a design could not be analysed against a library"

# The counter's parameter decides only its widths, so it is one generic unit, specialised for each
# width when a design is elaborated (DESIGN_GUIDE.md section 18, M5.1): the producer's library
# holds it once, named as the source names it, and the consumer stores nothing of it, since a
# second copy would make an unqualified reference ambiguous.
producer=$("$enzan" lib list libs/pc)
grep -qx '  pcounter' <<<"$producer" \
    || fail "the producer's library does not hold the counter as one unit: $producer"
grep -q 'pcounter\$' <<<"$producer" && fail "the producer's library holds a specialisation: $producer"
"$enzan" lib list libs/pw | grep -q 'pcounter' \
    && fail "the consumer stored a unit its input library already held"

# A type parameter keeps a unit specialised, one per parameterisation and named for its values,
# not by a counter: a counter would number them in the order one compilation walked them, and
# the same name would mean different modules in two libraries. The consumer stores the one it
# created, and not the one the producer already has.
grep -q 'ptyped\$T_logic_3_0' <<<"$producer" \
    || fail "the producer's library does not name its parameterisation: $producer"
"$enzan" lib list libs/pw | grep -q 'ptyped\$T_logic_7_0' \
    || fail "the consumer's library does not hold the parameterisation it created"
"$enzan" lib list libs/pw | grep -q 'ptyped\$T_logic_3_0' \
    && fail "the consumer duplicated a parameterisation its input library already held"

par_output=$("$enzan" run --in-lib libs/pc --in-lib libs/pw --timescale ns pwrap_tb) \
    || fail "a parametrised design split across libraries did not run"
grep -q 'parametrisable ok' <<<"$par_output" || fail "the parametrised run did not check out"

# A parameterised top is named by its parameter values, so the name the user wrote has to find
# it; otherwise `run <top>` could not name a parameterised testbench at all.
"$enzan" run --in-lib libs/pw --in-lib libs/pc --timescale ns pwrap_tb >/dev/null \
    || fail "the libraries could not be given in the other order"

# --- the socket a served kernel binds ---------------------------------------------------------------

# A signal that is not handled ends a process without running a destructor, so the listener's own
# cleanup never happens and the socket is left on disk. SIGTERM is handled for that reason.
sock="$work/served.sock"
"$enzan" --listen "$sock" --top tb counter.sv >/dev/null 2>&1 &
served=$!
for _ in $(seq 200); do [[ -S $sock ]] && break; sleep 0.05; done
[[ -S $sock ]] || fail "a served kernel did not bind its socket"

kill -TERM "$served" 2>/dev/null
wait "$served" 2>/dev/null || true
sleep 0.3
[[ -e $sock ]] && fail "SIGTERM left the socket behind"

# SIGKILL cannot be delivered to a handler, so that one always leaves the path. It has to cost
# nothing: binding probes an existing path and reclaims one nothing is listening on.
"$enzan" --listen "$sock" --top tb counter.sv >/dev/null 2>&1 &
served=$!
for _ in $(seq 200); do [[ -S $sock ]] && break; sleep 0.05; done
kill -KILL "$served" 2>/dev/null
wait "$served" 2>/dev/null || true
sleep 0.3
[[ -e $sock ]] || fail "SIGKILL is expected to leave the path; the test below is then moot"

"$enzan" --listen "$sock" --top tb counter.sv >/dev/null 2>&1 &
served=$!
bound=no
for _ in $(seq 200); do [[ -S $sock ]] && { bound=yes; break; }; sleep 0.05; done
kill "$served" 2>/dev/null
wait "$served" 2>/dev/null || true
[[ $bound == yes ]] || fail "a socket left by a killed kernel was not reclaimed by the next one"

# --- --attach is a window's option ------------------------------------------------------------------

# Without --gui there is nothing to attach: silently ignoring it would start a second simulation
# while the user waited to be shown the one they meant.
[[ $(status_of "$enzan" --attach "$work/nothing.sock") -eq 1 ]] \
    || fail "--attach without --gui was not refused"

# --- cleaning keeps the library and removes its entries -------------------------------------------

"$enzan" lib clean libs/work >/dev/null || fail "a library could not be cleaned"
[[ -f libs/work/enzan.lib ]] || fail "cleaning removed the library itself"
"$enzan" lib list libs/work | grep -q '0 units' || fail "cleaning left entries behind"

echo "PASS library"
