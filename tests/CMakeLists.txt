# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites (DESIGN_GUIDE.md section 14).

include(GoogleTest)

add_subdirectory(unit)

# The cocotb golden tests, one for each frontend's toplevel, drive Enzan as a user does
# (DESIGN_GUIDE.md section 14.6). Skipped rather than failed when no Python has cocotb installed,
# so that a contributor without it can still run the suite; CI installs it. ENZAN_COCOTB_PYTHON
# names the Python to use when the one CMake finds is not the one cocotb was installed into.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(ENZAN_COCOTB_PYTHON
    "${Python3_EXECUTABLE}"
    CACHE FILEPATH "A Python that has cocotb installed, for the cocotb golden tests")
set(ENZAN_HAVE_COCOTB OFF)
if(ENZAN_COCOTB_PYTHON)
  execute_process(
    COMMAND "${ENZAN_COCOTB_PYTHON}" -c "import cocotb"
    RESULT_VARIABLE _enzan_cocotb_missing
    OUTPUT_QUIET ERROR_QUIET)
  if(_enzan_cocotb_missing EQUAL 0)
    set(ENZAN_HAVE_COCOTB ON)
    message(STATUS "The cocotb golden tests will use ${ENZAN_COCOTB_PYTHON}")
  endif()
endif()
if(NOT ENZAN_HAVE_COCOTB)
  message(STATUS "cocotb not found; the cocotb golden tests are skipped")
endif()

# Command lines that are wrong, and what Enzan says of them (DESIGN_GUIDE.md section 8). Every
# build runs it, since a command line is checked before any frontend is reached.
add_test(NAME golden.cli COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/golden/run-cli-check.sh"
                                 $<TARGET_FILE:enzan>)
set_tests_properties(golden.cli PROPERTIES PASS_REGULAR_EXPRESSION "PASS cli" TIMEOUT 120)

# Golden tests need the simulator, so they exist only when a frontend does.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_subdirectory(golden)
endif()
if(ENZAN_ENABLE_VHDL)
  add_subdirectory(golden/vhdl)
endif()
if(ENZAN_ENABLE_SYSTEMVERILOG AND ENZAN_ENABLE_VHDL)
  add_subdirectory(golden/mixed)
endif()
# add_subdirectory(conformance)  # M5 — sv-tests, ivtest harnesses
