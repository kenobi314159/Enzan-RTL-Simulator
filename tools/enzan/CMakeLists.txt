# SPDX-License-Identifier: GPL-3.0-or-later
#
# The headless kernel. It must remain usable with no display attached
# (DESIGN_GUIDE.md section 2.2), so it must never gain a dependency on Qt or on
# anything under src/gui.

add_executable(enzan Main.cpp)
target_link_libraries(enzan PRIVATE enzan::compile_options enzan::common)

# Simulating needs a frontend, and either language's will do.
if(ENZAN_ENABLE_SYSTEMVERILOG OR ENZAN_ENABLE_VHDL)
  target_link_libraries(enzan PRIVATE enzan::elab enzan::codegen enzan::runtime enzan::wavedb
                                      enzan::ipc_server)
endif()
if(ENZAN_ENABLE_SYSTEMVERILOG)
  target_link_libraries(enzan PRIVATE enzan::frontend_sv)
endif()

target_link_libraries(enzan PRIVATE enzan::session)

# VPI applications loaded with `--vpi` resolve the routines against this executable.
enzan_export_vpi(enzan)

install(
  TARGETS enzan
  RUNTIME DESTINATION "${ENZAN_INSTALL_BINDIR}"
  COMPONENT enzan)
