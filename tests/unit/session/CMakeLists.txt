# SPDX-License-Identifier: GPL-3.0-or-later

# The session compiles and runs real SystemVerilog, which is the point of testing it at all:
# its job is to hold the three phases of section 2.1 together. Without the frontend there is
# nothing for it to hold.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  enzan_add_unit_test(
    session
    SOURCES ConstantFunctionRunnerTests.cpp FileListTests.cpp SessionTests.cpp
    DEPENDS enzan::session)

  enzan_use_test_plugins(enzan_unittest_session)
endif()
