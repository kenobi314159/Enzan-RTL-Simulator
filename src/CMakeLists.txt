# SPDX-License-Identifier: GPL-3.0-or-later
#
# Components are added as the milestones that introduce them land
# (DESIGN_GUIDE.md sections 12 and 18). The commented entries are the planned
# set, kept here so the intended shape of the tree is visible from the build
# file rather than only from the design guide.

add_subdirectory(common)

add_subdirectory(eir)
add_subdirectory(frontend)
add_subdirectory(elab)
add_subdirectory(codegen)
add_subdirectory(runtime)
add_subdirectory(wavedb)
add_subdirectory(ipc)
if(ENZAN_BUILD_GUI)
  add_subdirectory(gui)
endif()
# add_subdirectory(control)   # M3 — Tcl interpreter and commands
# add_subdirectory(pli)       # M4 — VPI and VHPI
