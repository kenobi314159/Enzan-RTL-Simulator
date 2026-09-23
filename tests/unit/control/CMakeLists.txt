# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  control
  SOURCES CompileOrderTests.cpp FileListTests.cpp InterpreterTests.cpp
  DEPENDS enzan::control)

# The session compiles and runs real SystemVerilog, which is the point of testing it at all:
# its job is to hold the three phases of section 2.1 together. Without the frontend there is
# nothing for it to hold.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  enzan_add_unit_test(
    control_session
    SOURCES CommandsTests.cpp SessionTests.cpp
    DEPENDS enzan::control enzan::control_session)
endif()
