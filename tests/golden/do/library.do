# SPDX-License-Identifier: GPL-3.0-or-later
#
# Driven twice against one library by run-library-check.sh. Everything it prints must be the
# same whether the units came from the frontend or from the library, because that is what
# "reused" has to mean (DESIGN_GUIDE.md section 4.4).
set source  [lindex $argv 0]
set library [lindex $argv 1]

vlib $library
vlog -sv $source
vsim -t ns tb

run -all

puts "finished at [examine -radix decimal /tb/count]"
puts "hierarchy: [find instances *]"
quit
