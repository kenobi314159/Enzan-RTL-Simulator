#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Makefile of DESIGN_GUIDE.md section 8.3, actually run. What it has to show is the part a
# shell script cannot assert for itself: that `make` rebuilds the right library and only that
# one, which depends on the dependency file naming every file an analysis read — the includes
# included, since those are what a build cannot work out for itself.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <enzan>" >&2
    exit 2
fi

enzan=$1
command -v make >/dev/null || { echo "PASS make (skipped: no make)"; exit 0; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() { echo "FAIL make: $1" >&2; exit 1; }

mkdir -p rtl/inc

cat >rtl/inc/widths.svh <<'EOF'
`define COUNTER_WIDTH 4
EOF

cat >rtl/counter.sv <<'EOF'
`include "widths.svh"
module counter (output logic [`COUNTER_WIDTH-1:0] q);
    initial q = '1;
endmodule
EOF

cat >rtl/tb.sv <<'EOF'
`include "widths.svh"
module tb;
    logic [`COUNTER_WIDTH-1:0] q;
    counter u_counter (.q(q));
    initial begin
        #1;
        $display("PASS make q=%0d", q);
        $finish;
    end
endmodule
EOF

printf '+incdir+rtl/inc\nrtl/counter.sv\n' >rtl/counter.f
printf '+incdir+rtl/inc\nrtl/tb.sv\n' >rtl/tb.f

cat >Makefile <<'EOF'
ENZAN ?= enzan

libs/%/.created:
	$(ENZAN) lib create libs/$*
	@touch $@

libs/counter/.stamp: rtl/counter.f | libs/counter/.created
	$(ENZAN) analyse --out-lib libs/counter -f rtl/counter.f \
	         --depfile libs/counter/deps.d --depfile-target $@
	@touch $@

libs/tb/.stamp: rtl/tb.f | libs/tb/.created
	$(ENZAN) analyse --in-lib libs/counter --out-lib libs/tb -f rtl/tb.f \
	         --depfile libs/tb/deps.d --depfile-target $@
	@touch $@

-include $(wildcard libs/*/deps.d)

STAMPS := libs/counter/.stamp libs/tb/.stamp

sim: $(STAMPS)
	$(ENZAN) run --in-lib libs/counter --in-lib libs/tb --timescale ns tb

check: $(STAMPS)
	$(ENZAN) elaborate --in-lib libs/counter --in-lib libs/tb tb
EOF

run_make() { make ENZAN="$enzan" "$@" 2>&1; }

# --- a cold build, then a warm one that does nothing ---------------------------------------------

out=$(run_make sim) || { echo "$out" >&2; fail "the cold build did not run"; }
grep -q 'PASS make q=15' <<<"$out" || fail "the simulation did not produce its check: $out"

out=$(run_make sim) || { echo "$out" >&2; fail "the warm build failed"; }
grep -q 'analyse' <<<"$out" && fail "nothing changed and make analysed anyway"

# --- editing a source rebuilds its library and not the other -------------------------------------

touch rtl/tb.sv
out=$(run_make sim) || { echo "$out" >&2; fail "the rebuild after touching a source failed"; }
grep -q 'out-lib libs/tb' <<<"$out" || fail "touching tb.sv did not rebuild libs/tb"
grep -q 'out-lib libs/counter' <<<"$out" && fail "touching tb.sv rebuilt libs/counter as well"


# --- editing an include rebuilds the library that included it ------------------------------------

# This is what the dependency file exists for: nothing in the Makefile names widths.svh, so a
# build that did not learn about it from `--depfile` would silently not rebuild here.
cat >rtl/inc/widths.svh <<'EOF'
`define COUNTER_WIDTH 8
EOF

out=$(run_make sim) || { echo "$out" >&2; fail "the rebuild after editing an include failed"; }
grep -q 'out-lib libs/counter' <<<"$out" \
    || fail "editing an included header did not rebuild the library that included it"

# And the new width reaches the simulation, so the rebuild was real rather than a touched stamp.
grep -q 'PASS make q=255' <<<"$out" \
    || fail "the rebuilt design still has the old width: $out"

# --- a deleted header does not wedge the build ---------------------------------------------------

rm rtl/inc/widths.svh
out=$(run_make sim 2>&1) && fail "a design whose header was deleted still built"
grep -q 'No rule to make target' <<<"$out" \
    && fail "make refused to run instead of letting the tool report the missing header"

echo "PASS make"
