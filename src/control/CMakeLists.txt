# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two components from one directory, for the reason src/control/README.md gives: the
# interpreter is shared with the GUI, which must not link the simulation kernel
# (DESIGN_GUIDE.md section 2.2).

enzan_add_component(
  control
  SOURCES CompileOrder.cpp FileList.cpp Interpreter.cpp
  HEADERS CompileOrder.h FileList.h Interpreter.h
  DEPENDS enzan::common)

target_include_directories(enzan_control SYSTEM PRIVATE ${TCL_INCLUDE_PATH})
target_link_libraries(enzan_control PRIVATE ${TCL_LIBRARY})

# The command vocabulary and the session it drives. Separate from `control` because it owns a
# kernel: the GUI links the interpreter for its theme and must not link the simulator
# (DESIGN_GUIDE.md section 2.2, and src/control/README.md).
enzan_add_component(
  control_session
  SOURCES Commands.cpp Session.cpp
  HEADERS Commands.h Session.h
  DEPENDS enzan::control enzan::eir enzan::elab enzan::runtime enzan::wavedb
  PRIVATE_DEPENDS enzan::codegen)

if(ENZAN_ENABLE_SYSTEMVERILOG)
  target_link_libraries(enzan_control_session PRIVATE enzan::frontend_sv)
endif()
