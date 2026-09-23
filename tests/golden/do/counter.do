# SPDX-License-Identifier: GPL-3.0-or-later
#
# M3's exit criterion: a design driven end to end from a script (DESIGN_GUIDE.md section 18).
#
# Everything here is the control vocabulary of section 8.1 and nothing is a command-line
# switch, which is the point — the script is the whole interface.
#
# The source and the file to write are passed in, because a golden test knows where the tree
# is and a script committed with a path in it would only work from one directory.
set source [lindex $argv 0]
set waves  [lindex $argv 1]

vlog -sv $source
vsim -t ns tb

log -r /tb
vcd file $waves

# A short run first, so that `examine` is reading a design part way through rather than one
# that has already finished.
run 100 ns
set halfway [examine -radix decimal /tb/count]
if {$halfway == "x" || $halfway == ""} {
    puts "FAIL counter.do: count did not read as a number part way through"
    quit -code 1
}

# A collapsed port is reachable under both its names, and `find` must see the ones inside the
# instance as well as the ones in the testbench.
if {[llength [find signals /tb/u_dut/*]] == 0} {
    puts "FAIL counter.do: nothing found inside /tb/u_dut"
    quit -code 1
}

# Forcing must beat the design, and releasing must let it drive again.
force /tb/enable 1'b0
set before [examine -radix decimal /tb/count]
run 50 ns
if {[examine -radix decimal /tb/count] != $before} {
    puts "FAIL counter.do: the counter moved while enable was forced low"
    quit -code 1
}
noforce /tb/enable

restart
run -all

puts "PASS counter.do"
quit
