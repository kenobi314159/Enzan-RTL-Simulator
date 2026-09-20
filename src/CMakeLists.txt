# SPDX-License-Identifier: GPL-3.0-or-later
#
# Components are added as the milestones that introduce them land
# (DESIGN_GUIDE.md sections 12 and 18). The commented entries are the planned
# set, kept here so the intended shape of the tree is visible from the build
# file rather than only from the design guide.

add_subdirectory(common)

# add_subdirectory(eir)       # M1 — the internal representation
# add_subdirectory(frontend)  # M1 (sv), M6 (vhdl)
# add_subdirectory(elab)      # M1 — elaboration and state layout
# add_subdirectory(codegen)   # M1 — EIR-B to bytecode
# add_subdirectory(runtime)   # M1 — VM, scheduler, value engine
# add_subdirectory(wavedb)    # M1 — waveform database, VCD and FST
# add_subdirectory(ipc)       # M2 — kernel/GUI protocol
# add_subdirectory(gui)       # M2 — Qt 6 application
# add_subdirectory(control)   # M3 — Tcl interpreter and commands
# add_subdirectory(pli)       # M4 — VPI and VHPI
