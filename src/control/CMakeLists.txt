# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two components from one directory, for the reason src/control/README.md gives: the
# interpreter is shared with the GUI, which must not link the simulation kernel
# (DESIGN_GUIDE.md section 2.2).

enzan_add_component(
  control
  SOURCES FileList.cpp Interpreter.cpp
  HEADERS FileList.h Interpreter.h
  DEPENDS enzan::common)

target_include_directories(enzan_control SYSTEM PRIVATE ${TCL_INCLUDE_PATH})
target_link_libraries(enzan_control PRIVATE ${TCL_LIBRARY})
