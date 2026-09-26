# SPDX-License-Identifier: GPL-3.0-or-later
#
# Source debugging from the control language (DESIGN_GUIDE.md section 10.4): a breakpoint on
# an HDL line, stepping by line, and `examine` on a stopped design.
set source [lindex $argv 0]

vlog -sv $source
vsim -t ns tb

# The line the counter increments on. Named by number rather than searched for, because a
# test that hunts for its own breakpoint line would still pass if every instruction were
# attributed to line one.
set counting_line 19

set id [bp counter.sv $counting_line]
if {$id == 0} {
    puts "FAIL debug.do: the breakpoint was not set"
    quit -code 1
}

run -all

# The design stopped somewhere short of the end, which is what a breakpoint is for.
if {[llength [bp]] == 0} {
    puts "FAIL debug.do: the breakpoint was not listed"
    quit -code 1
}

# A stopped design is readable, and that is the whole point of stopping.
set here [examine -radix decimal /tb/count]
if {$here == ""} {
    puts "FAIL debug.do: could not examine a stopped design"
    quit -code 1
}

# Stepping moves on without running to the end.
step
step

# Disabling it lets the run finish.
disablebp $id
run -all

puts "PASS debug.do"
quit
