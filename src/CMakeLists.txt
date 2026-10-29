# SPDX-License-Identifier: GPL-3.0-or-later
#
# The components, each added after those it depends on.

add_subdirectory(common)

add_subdirectory(eir)
add_subdirectory(frontend)
add_subdirectory(elab)
add_subdirectory(codegen)
add_subdirectory(runtime)
add_subdirectory(wavedb)
add_subdirectory(pli)

# Before `ipc`, because the server end of the protocol serves a session.
add_subdirectory(session)
add_subdirectory(ipc)
if(ENZAN_BUILD_GUI)
  add_subdirectory(gui)
endif()
