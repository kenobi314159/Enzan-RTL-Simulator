# SPDX-License-Identifier: GPL-3.0-or-later
#
# cocotb's standard named-library variables, with a separately stored main VHDL library.

VHDL_LIB_ORDER = ip
VHDL_SOURCES_ip = $(PWD)/counter.vhd
VHDL_SOURCES = $(PWD)/top.vhd
COCOTB_TOPLEVEL = top
COCOTB_TEST_MODULES = test_top

ENZAN_OUT_LIB = $(SIM_BUILD)/enzan-libraries/work

include $(shell cocotb-config --makefiles)/Makefile.sim
