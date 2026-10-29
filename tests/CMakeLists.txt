# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suites.

include(GoogleTest)

add_subdirectory(unit)

# The cocotb golden tests, one for each frontend's toplevel, drive Enzan as a user does. Skipped
# rather than failed when no Python has cocotb installed, so that a contributor without it can
# still run the suite; CI installs it. ENZAN_COCOTB_PYTHON names the Python to use when the one
# CMake finds is not the one cocotb was installed into.
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

# What the cocotb golden tests change in a sanitiser run's environment. Each runs CPython inside
# Enzan, and neither CPython nor cocotb frees everything it allocated before the process exits,
# so leak checking is off for those runs alone; ASan and UBSan still check everything else. A
# leak suppression could not be narrower: it matches any frame of a stack, and whatever Enzan
# allocates for cocotb has Python's frames above it. Appended, so that it overrides the
# `detect_leaks=1` the run sets.
set(ENZAN_COCOTB_ENVIRONMENT "ASAN_OPTIONS=path_list_append:detect_leaks=0")

# Command lines that are wrong, and what Enzan says of them. Every build runs it, since a
# command line is checked before any frontend is reached.
add_test(NAME golden.cli COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/golden/run-cli-check.sh"
                                 $<TARGET_FILE:enzan> "${ENZAN_FRONT_END_FROM_PROGRAM}")
set_tests_properties(golden.cli PROPERTIES PASS_REGULAR_EXPRESSION "PASS cli" TIMEOUT 120)

# This build installed, moved and made read-only, run as the build is, and installing nothing that
# is not Enzan's. Every build runs it, since every build installs.
add_test(
  NAME golden.install
  COMMAND
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/run-install-check.sh" "${CMAKE_COMMAND}"
    "${PROJECT_BINARY_DIR}" "$<IF:$<TARGET_EXISTS:enzan-gui>,ON,OFF>" "${ENZAN_INSTALL_BINDIR}"
    "${ENZAN_INSTALL_LIBEXECDIR}" "${ENZAN_INSTALL_VHDLDIR}" "${ENZAN_INSTALL_INCLUDEDIR}"
    "${ENZAN_INSTALL_DATADIR}")
set_tests_properties(golden.install PROPERTIES PASS_REGULAR_EXPRESSION "PASS install" TIMEOUT 120)

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
