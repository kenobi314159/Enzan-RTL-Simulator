# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  ipc
  SOURCES KernelServerTests.cpp ProtocolTests.cpp TransportTests.cpp
  DEPENDS enzan::ipc enzan::ipc_server)

enzan_use_test_plugins(enzan_unittest_ipc)

# A design's objects are listed from its sources too, through each frontend the build has, and
# a VHDL one is analysed against the standard libraries the build analysed.
if(ENZAN_ENABLE_VHDL)
  add_dependencies(enzan_unittest_ipc enzan-vhdl-libraries)
  target_compile_definitions(enzan_unittest_ipc
                             PRIVATE ENZAN_VHDL_LIBRARY_ROOT="${ENZAN_VHDL_LIBRARY_ROOT}")
endif()
