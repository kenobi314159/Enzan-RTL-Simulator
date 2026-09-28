#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `listen` hands a session to a front end (DESIGN_GUIDE.md section 8.1), which is a property of
# a process that does not exit, so it cannot be checked from inside a `do` script the way the
# rest of the vocabulary is: the command blocks, and a script that reached the line after it
# would be proof that it had not worked.
#
# What is checked here is the driver wiring — that a script can set a simulation up and then
# leave it waiting for a window, by either spelling, and that the refusals fire. The socket
# appearing is the observable: it is bound only after the script has run, so a bound socket is
# the whole sequence having happened in the right order.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <enzan> <source.sv>" >&2
    exit 2
fi

enzan=$1
source_file=$2

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
    echo "FAIL listen: $1" >&2
    exit 1
}

# Waits for the kernel to bind, then stops it. A poll rather than a fixed sleep, because
# elaboration takes as long as it takes and a sleep long enough for a slow machine would make
# every run of this test pay for it.
serves() {
    local socket=$1 pid=$2 waited=0
    while ((waited < 200)); do
        [[ -S $socket ]] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.05
        ((waited += 1))
    done
    return 1
}

# --- the command, placed by the script ------------------------------------------------------

cat >"$work/command.do" <<'SCRIPT'
vlog -sv [lindex $argv 0]
vsim -t ns tb
log -r /tb
listen [lindex $argv 1]
puts "CARRIED ON"
SCRIPT

"$enzan" --do "$work/command.do" "$source_file" "$work/command.sock" >"$work/command.log" 2>&1 &
command_pid=$!
serves "$work/command.sock" "$command_pid" \
    || { cat "$work/command.log" >&2; fail "the \`listen\` command did not bind a socket"; }
kill "$command_pid" 2>/dev/null || true
wait "$command_pid" 2>/dev/null || true

grep -q 'attach with' "$work/command.log" \
    || fail "nothing told the user how to attach to the socket that was bound"

# The line after `listen` must not have run. `listen` returns only when a front end asks for the
# simulation to *stop* — detaching leaves the kernel serving, which is what section 2.2 requires
# of it — and this kernel was killed while still waiting, so nothing had asked for anything.
if grep -q 'CARRIED ON' "$work/command.log"; then
    fail "the script ran past \`listen\` while the session was still being served"
fi

# --- the option, alongside a script ---------------------------------------------------------

cat >"$work/option.do" <<'SCRIPT'
vlog -sv [lindex $argv 0]
vsim -t ns tb
log -r /tb
SCRIPT

"$enzan" --do "$work/option.do" --listen "$work/option.sock" "$source_file" \
    >"$work/option.log" 2>&1 &
option_pid=$!
serves "$work/option.sock" "$option_pid" \
    || { cat "$work/option.log" >&2; fail "--listen beside --do did not bind a socket"; }
kill "$option_pid" 2>/dev/null || true
wait "$option_pid" 2>/dev/null || true

# --- a design that has already run -----------------------------------------------------------

# Refused rather than served, because serving restarts from time zero: a session that had run
# would be reset and the run the user meant to look at would be gone.
cat >"$work/late.do" <<'SCRIPT'
vlog -sv [lindex $argv 0]
vsim -t ns tb
run 50 ns
listen [lindex $argv 1]
SCRIPT

if "$enzan" --do "$work/late.do" "$source_file" "$work/late.sock" >"$work/late.log" 2>&1; then
    fail "serving a design that had already run was allowed"
fi
grep -q 'already run' "$work/late.log" \
    || { cat "$work/late.log" >&2; fail "the refusal did not say that the design had run"; }
[[ -S $work/late.sock ]] && fail "a socket was bound for a design that had already run"

# --- a design that has not been elaborated ---------------------------------------------------

cat >"$work/early.do" <<'SCRIPT'
listen [lindex $argv 1]
SCRIPT

if "$enzan" --do "$work/early.do" "$source_file" "$work/early.sock" >"$work/early.log" 2>&1; then
    fail "serving a session with no simulation was allowed"
fi
grep -q 'vsim' "$work/early.log" \
    || { cat "$work/early.log" >&2; fail "the refusal did not point at \`vsim\`"; }

echo "PASS listen"
