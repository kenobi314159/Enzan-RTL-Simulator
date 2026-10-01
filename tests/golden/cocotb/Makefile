# SPDX-License-Identifier: GPL-3.0-or-later
#
# A cocotb testbench, written as any cocotb user writes one; run with SIM=enzan.

VERILOG_SOURCES = $(PWD)/counter.sv
COCOTB_TOPLEVEL = counter
COCOTB_TEST_MODULES = test_counter

include $(shell cocotb-config --makefiles)/Makefile.sim
