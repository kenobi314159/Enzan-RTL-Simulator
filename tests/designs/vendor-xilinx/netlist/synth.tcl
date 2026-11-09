# SPDX-License-Identifier: GPL-3.0-or-later
#
# Synthesises NEORV32 for a 7-series part, out of context, and writes its functional-simulation
# netlists in Verilog and VHDL. make.sh runs it:
#
#   vivado -mode batch -source synth.tcl -tclargs <neorv32-source> <wrappers> <output-directory>
#
# The sources are NEORV32's own file list and the top wrap.py writes into <wrappers>, which
# configures NEORV32 as its testbench does.

lassign $argv source wrappers out
set_param general.maxThreads 4

set list [open $source/rtl/file_list_core.f]
set files {}
foreach line [split [read $list] "\n"] {
    set line [string trim $line]
    if {$line ne ""} { lappend files [string map [list {$NEORV32_HOME} $source] $line] }
}
close $list
read_vhdl -vhdl2008 -library neorv32 $files
read_vhdl -vhdl2008 -library neorv32 \
    [list $wrappers/neorv32_netlist_pkg.vhd $wrappers/neorv32_netlist.vhd]

synth_design -top neorv32_netlist -part xc7a200tfbg676-2 -mode out_of_context

# Every module below the top is renamed, so that none has the name of an entity of NEORV32's,
# which a simulation of the netlist under NEORV32's testbench reads beside it: neorv32_top, above
# all, is wrap.py's stand-in around the netlist there.
rename_ref -prefix_all netlist_

report_utilization -file $out/utilization.txt
write_verilog -force -mode funcsim $out/neorv32_funcsim.v
write_vhdl -force -mode funcsim $out/neorv32_funcsim.vhd
