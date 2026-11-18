# SPDX-License-Identifier: GPL-3.0-or-later
#
# A stored SystemVerilog top consuming an existing Enzan library. The included header is left to
# Enzan's dependency file rather than named as a Make prerequisite.

ENZAN := $(if $(ENZAN_BIN_DIR),$(ENZAN_BIN_DIR)/enzan,enzan)
SIM_BUILD ?= sim_build

IP_LIBRARY := $(SIM_BUILD)/ip
IP_STAMP := $(IP_LIBRARY)/.stamp

$(IP_STAMP): $(PWD)/counter.sv | $(SIM_BUILD)
	$(ENZAN) analyse --timescale ps --out-lib $(IP_LIBRARY) $<
	@touch $@

VERILOG_SOURCES = $(PWD)/top.sv
VERILOG_INCLUDE_DIRS = $(PWD)/include
COCOTB_TOPLEVEL = top
COCOTB_TEST_MODULES = test_top

ENZAN_INPUT_LIBRARIES = $(IP_LIBRARY)
ENZAN_OUT_LIB = $(SIM_BUILD)/enzan-libraries/work
CUSTOM_COMPILE_DEPS += $(IP_STAMP)

include $(shell cocotb-config --makefiles)/Makefile.sim
