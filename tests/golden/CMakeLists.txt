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

# A failure line, anchored to the start of one.
#
# The word on its own is not enough. A diagnostic quotes the source line it points at, and a
# case that deliberately writes an out-of-range select has a `$display("FAIL: ...")` on the
# very line being quoted — so the transcript contains the word without anything having failed.
# A real failure is printed by the design and starts its line; a quoted one is indented behind
# a line-number gutter.
set(ENZAN_GOLDEN_FAILURE_PATTERN "\nFAIL")

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
               FAIL_REGULAR_EXPRESSION "${ENZAN_GOLDEN_FAILURE_PATTERN}"
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

# Drivers of disjoint bits of one signal, the case section 3.7 is written around: a
# parameterised number of instances whose outputs are bits and slices of one array.
enzan_add_golden_test(drivers)

# What each kind of net holds where nothing drives it.
enzan_add_golden_test(nets)

# Nets with several drivers, resolved per bit: tristate buses, wired logic, pulls and supplies.
enzan_add_golden_test(resolution)

# The comparisons with don't-care bits: ==?, casez and casex.
enzan_add_golden_test(wildcards)

# `inside` and `case ... inside`, which Icarus Verilog does not implement, so the case checks
# itself against the values IEEE 1800 sections 11.4.13 and 12.5.4 give.
enzan_add_golden_test(inside NO_DIFFERENTIAL)

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

# The command line of section 8, driven as a Makefile drives it. A library exists so that what
# one process analysed another can run, so the separation is what has to be checked and cannot
# be checked from inside a single invocation.
add_test(NAME golden.library
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-library-check.sh" $<TARGET_FILE:enzan>
                 "${CMAKE_CURRENT_SOURCE_DIR}/sv/counter.sv")
set_tests_properties(
  golden.library
  PROPERTIES PASS_REGULAR_EXPRESSION "PASS library"
             FAIL_REGULAR_EXPRESSION "${ENZAN_GOLDEN_FAILURE_PATTERN}"
             TIMEOUT 120)

# The Makefile of section 8.3, actually run. What only `make` can show is that the right library
# rebuilds and no other, which rests on the dependency file naming the headers an analysis read
# — the one thing a build cannot work out for itself.
add_test(NAME golden.make
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-make-check.sh" $<TARGET_FILE:enzan>)
set_tests_properties(
  golden.make
  PROPERTIES PASS_REGULAR_EXPRESSION "PASS make"
             FAIL_REGULAR_EXPRESSION "${ENZAN_GOLDEN_FAILURE_PATTERN}"
             TIMEOUT 180)

# cocotb driving Enzan through VPI, as a user runs it (DESIGN_GUIDE.md section 14.6). Skipped
# rather than failed when no Python has cocotb installed, so that a contributor without it can
# still run the suite; CI installs it. ENZAN_COCOTB_PYTHON names the Python to use when the one
# CMake finds is not the one cocotb was installed into.
set(ENZAN_COCOTB_PYTHON
    "${Python3_EXECUTABLE}"
    CACHE FILEPATH "A Python that has cocotb installed, for the cocotb golden test")
if(ENZAN_COCOTB_PYTHON)
  execute_process(
    COMMAND "${ENZAN_COCOTB_PYTHON}" -c "import cocotb"
    RESULT_VARIABLE _enzan_cocotb_missing
    OUTPUT_QUIET ERROR_QUIET)
endif()
if(ENZAN_COCOTB_PYTHON AND _enzan_cocotb_missing EQUAL 0)
  message(STATUS "The cocotb golden test will use ${ENZAN_COCOTB_PYTHON}")
  add_test(NAME golden.cocotb
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-cocotb-check.sh" $<TARGET_FILE:enzan>
                   "${ENZAN_COCOTB_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/cocotb")
  set_tests_properties(
    golden.cocotb PROPERTIES PASS_REGULAR_EXPRESSION "PASS cocotb" FAIL_REGULAR_EXPRESSION
                             "FAIL=[1-9]" TIMEOUT 300)
else()
  message(STATUS "cocotb not found; the cocotb golden test is skipped")
endif()
