# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites (DESIGN_GUIDE.md section 14).

include(GoogleTest)

add_subdirectory(unit)

# Golden tests need the simulator, so they exist only when a frontend does.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_subdirectory(golden)
endif()
if(ENZAN_ENABLE_VHDL)
  add_subdirectory(golden/vhdl)
endif()
# add_subdirectory(conformance)  # M5 — sv-tests, ivtest harnesses
