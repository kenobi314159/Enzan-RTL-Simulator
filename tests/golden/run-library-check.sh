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
"$enzan" analyse --out-lib libs/work -f rtl.f || fail "analysis into a library failed"

listing=$("$enzan" lib list libs/work)
grep -q "library 'work'" <<<"$listing" || fail "the library does not report its own name"
grep -q 'counter' <<<"$listing" || fail "the library does not hold what was analysed into it"

# The entry is named after its source, so a reader can see what is in the directory.
ls libs/work | grep -q 'counter.sv' || fail "an entry is not named after the source it holds"

run_output=$("$enzan" run --in-lib libs/work --timescale ns tb) \
    || fail "a design analysed by one process did not run in another"
grep -q 'PASS counter' <<<"$run_output" || fail "the run did not reach the design's own check"

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
"$enzan" analyse --out-lib libs/one split/one.sv || fail "libs/one could not be analysed"
"$enzan" analyse --out-lib libs/two split/two.sv || fail "libs/two could not be analysed"

both=$("$enzan" run --in-lib libs/one --in-lib libs/two --timescale ns two_tb) \
    || fail "two libraries could not be combined"
grep -q 'PASS split' <<<"$both" || fail "the combined run did not reach the design's check"

# One name in two libraries is reported rather than silently resolved (8.2).
"$enzan" lib create libs/clash >/dev/null
"$enzan" analyse --out-lib libs/clash split/two.sv || fail "libs/clash could not be analysed"

clash=$("$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two_tb 2>&1 || true)
[[ $(status_of "$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two_tb) -ne 0 ]] \
    || fail "a name defined by two libraries was resolved rather than reported"
grep -q 'more than one library' <<<"$clash" \
    || fail "the collision was not reported as one: $clash"

# Qualifying it says which was meant.
"$enzan" run --in-lib libs/two --in-lib libs/clash --timescale ns two.two_tb >/dev/null \
    || fail "a qualified name did not resolve the collision"

# --- cleaning keeps the library and removes its entries -------------------------------------------

"$enzan" lib clean libs/work >/dev/null || fail "a library could not be cleaned"
[[ -f libs/work/enzan.lib ]] || fail "cleaning removed the library itself"
"$enzan" lib list libs/work | grep -q '0 units' || fail "cleaning left entries behind"

echo "PASS library"
