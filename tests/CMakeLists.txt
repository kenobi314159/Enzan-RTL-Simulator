# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites (DESIGN_GUIDE.md section 14).

include(GoogleTest)

add_subdirectory(unit)

# Golden tests need the simulator, so they exist only when the frontend does.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_subdirectory(golden)
endif()
# add_subdirectory(conformance)  # M5 — sv-tests, ivtest harnesses
