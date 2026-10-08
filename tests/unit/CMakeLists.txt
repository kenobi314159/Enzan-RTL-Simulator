# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mirrors the src/ layout so that the test for a file is always findable at the
# corresponding path (DESIGN_GUIDE.md section 12).

add_subdirectory(support)

add_subdirectory(common)
add_subdirectory(eir)
add_subdirectory(codegen)
add_subdirectory(elab)
add_subdirectory(frontend)
add_subdirectory(runtime)
add_subdirectory(wavedb)
add_subdirectory(pli)
add_subdirectory(ipc)

add_subdirectory(session)

if(ENZAN_BUILD_GUI)
  add_subdirectory(gui)
endif()
