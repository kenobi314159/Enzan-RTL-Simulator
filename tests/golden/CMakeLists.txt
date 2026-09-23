# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-checking HDL cases (DESIGN_GUIDE.md section 14.2).
#
# Self-checking is strongly preferred over golden output files: a testbench that calls
# `$fatal` on mismatch needs no golden file to drift out of date. Every case here ends by
# printing `PASS <name>` and calling `$finish`, so a case that stops early — because a
# construct is unsupported, or because the kernel aborted — fails on the missing line rather
# than passing quietly.

# ---------------------------------------------------------------------------
# enzan_add_golden_test(<name> [NO_DIFFERENTIAL])
# ---------------------------------------------------------------------------
#
# Registers `golden.<name>`, which runs sv/<name>.sv and requires the PASS line, and
# `golden.<name>.differential`, which compares the transcript against a reference simulator.
#
# NO_DIFFERENTIAL marks a case where simulators legitimately differ, which section 14.3
# warns about. Such a case still runs under Enzan; it simply is not held to another tool's
# behaviour.
function(enzan_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NO_DIFFERENTIAL" "" "")

  set(source "${CMAKE_CURRENT_SOURCE_DIR}/sv/${name}.sv")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "golden test '${name}' has no source at ${source}")
  endif()

  add_test(NAME golden.${name} COMMAND enzan --top tb --timescale ns "${source}")
  set_tests_properties(
    golden.${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "PASS ${name}"
               FAIL_REGULAR_EXPRESSION "FAIL"
               TIMEOUT 60)

  if(arg_NO_DIFFERENTIAL OR NOT ENZAN_REFERENCE_SIMULATOR)
    return()
  endif()

  add_test(
    NAME golden.${name}.differential
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-differential.sh" $<TARGET_FILE:enzan>
            "${ENZAN_REFERENCE_SIMULATOR}" "${source}" tb)
  set_tests_properties(golden.${name}.differential PROPERTIES TIMEOUT 120)
endfunction()

# Differential testing is the single highest-value testing strategy available and must exist
# from M1 (section 14.3). It is skipped rather than failed when no reference is installed, so
# that a contributor without one can still run the suite; CI installs one.
find_program(ENZAN_REFERENCE_SIMULATOR iverilog)
if(ENZAN_REFERENCE_SIMULATOR)
  message(STATUS "Differential tests will compare against ${ENZAN_REFERENCE_SIMULATOR}")
else()
  message(STATUS "iverilog not found; differential tests are skipped")
endif()

# M1's exit criteria name these two by name.
enzan_add_golden_test(counter)
enzan_add_golden_test(fifo)

enzan_add_golden_test(operators)
enzan_add_golden_test(control_flow)

# Time zero is where two correct simulators differ most: Icarus Verilog does not evaluate
# `always_comb` there, where IEEE 1800 says it must.
enzan_add_golden_test(scheduling NO_DIFFERENTIAL)

# Depth, repetition and length, which the cases above deliberately do without: three levels of
# hierarchy below the testbench, two modules instantiated three times over at different
# parameters, and several thousand cycles of transfers with faults injected part way through.
# A short case cannot say whether a design's accounting stays straight over a long run, and a
# single-instance case cannot tell a parameter that is honoured from one that is ignored.
enzan_add_golden_test(router)

# ---------------------------------------------------------------------------
# Waveform output
# ---------------------------------------------------------------------------
#
# M1's exit criteria include waveforms opening in GTKWave (section 18). A viewer needs a display
# and cannot run in CI, so the properties a viewer relies on are checked instead — balanced
# scopes, declared identifiers, widths that match their declaration, monotonic time. That is both
# automatable and stricter than "it opened without complaining".
#
# `router` is checked as well as the two M1 cases because it is the only one with nested scopes
# and repeated instances, which is where a writer's scope nesting and its aliasing of collapsed
# ports actually get exercised.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  foreach(case IN ITEMS counter fifo router)
    add_test(
      NAME golden.${case}.vcd
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-vcd-check.sh" $<TARGET_FILE:enzan>
              "${CMAKE_CURRENT_SOURCE_DIR}/check-vcd.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/sv/${case}.sv" tb)
    set_tests_properties(golden.${case}.vcd PROPERTIES TIMEOUT 60)
  endforeach()
else()
  message(STATUS "Python 3 not found; the VCD structure checks are skipped")
endif()

# FST is validated by reading it back with GTKWave's own converter and comparing against the VCD
# written from the same run. That checks the file is readable, that its hierarchy survives
# including the aliases a collapsed port needs, and that the two writers agree about every value —
# which is a far stronger statement than either writer passing a check of its own.
find_program(ENZAN_FST2VCD fst2vcd)
if(Python3_Interpreter_FOUND AND ENZAN_FST2VCD)
  message(STATUS "FST output will be checked against ${ENZAN_FST2VCD}")

  foreach(case IN ITEMS counter fifo router)
    add_test(
      NAME golden.${case}.fst
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-fst-check.sh" $<TARGET_FILE:enzan>
              "${ENZAN_FST2VCD}" "${CMAKE_CURRENT_SOURCE_DIR}/check-vcd.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/sv/${case}.sv" tb)
    set_tests_properties(golden.${case}.fst PROPERTIES TIMEOUT 60)
  endforeach()
else()
  message(STATUS "fst2vcd not found; the FST round-trip checks are skipped")
endif()
