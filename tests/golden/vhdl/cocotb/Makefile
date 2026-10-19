# SPDX-License-Identifier: GPL-3.0-or-later
#
# A cocotb testbench of a VHDL toplevel, written as any cocotb user writes one; run with SIM=enzan
# TOPLEVEL_LANG=vhdl.

TOPLEVEL_LANG = vhdl
VHDL_SOURCES = $(PWD)/counter.vhd
COCOTB_TOPLEVEL = counter
COCOTB_TEST_MODULES = test_counter

include $(shell cocotb-config --makefiles)/Makefile.sim
