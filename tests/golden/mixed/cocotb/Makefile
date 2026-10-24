# SPDX-License-Identifier: GPL-3.0-or-later
#
# A cocotb testbench of a design of both languages, written as any cocotb user writes one; run with
# SIM=enzan and either TOPLEVEL_LANG, which chooses the toplevel's language.

TOPLEVEL_LANG ?= verilog

ifeq ($(TOPLEVEL_LANG),verilog)
    VERILOG_SOURCES = $(PWD)/sv_counter.sv $(PWD)/sv_top.sv
    VHDL_SOURCES = $(PWD)/vhdl_counter.vhd
    COCOTB_TOPLEVEL = sv_top
else
    VERILOG_SOURCES = $(PWD)/sv_counter.sv
    VHDL_SOURCES = $(PWD)/vhdl_counter.vhd $(PWD)/vhdl_top.vhd
    COCOTB_TOPLEVEL = vhdl_top
endif
COCOTB_TEST_MODULES = test_mixed

include $(shell cocotb-config --makefiles)/Makefile.sim
