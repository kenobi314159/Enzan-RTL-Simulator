ENZAN ?= enzan

LIB_DIR ?= rtl_libs

SIM_ARGS ?=

$(LIB_DIR)/.stamp: filelist.f
	$(ENZAN) analyse --timescale ns --out-lib $(LIB_DIR) -f filelist.f --depfile $(LIB_DIR)/deps.d --depfile-target $@
	@touch $@

synth: $(LIB_DIR)/.stamp

-include $(LIB_DIR)/deps.d

check: synth
	$(ENZAN) elaborate --in-lib $(LIB_DIR) tb

sim: synth
	$(ENZAN) run $(SIM_ARGS) --in-lib $(LIB_DIR) tb

clean:
	-rm -rf $(LIB_DIR)
