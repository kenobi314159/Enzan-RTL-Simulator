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

# Unpacked arrays, stored flat.
enzan_add_golden_test(arrays)

# Arrays and structs where Icarus Verilog cannot be the reference: whole-array copy and equality,
# and assignment patterns, which version 12 does not implement; an array through a port, which
# it leaves X; and an index past an inner dimension, which it lets land in the next row where
# IEEE 1800 section 7.4.6 makes it out of range.
enzan_add_golden_test(aggregates NO_DIFFERENTIAL)

# Functions and tasks, inlined into the processes that call them.
enzan_add_golden_test(subroutines)

# The static variables of functions and tasks, which every call shares.
enzan_add_golden_test(static_locals)

# Callers sharing a static function's scratch variable. Icarus Verilog makes the always_comb
# callers wait on it, and they wake one another without end.
enzan_add_golden_test(static_function_callers NO_DIFFERENTIAL)

# Hierarchical references, down, up and across, from a definition two instances share.
enzan_add_golden_test(hierarchical)

# Interfaces, with their own logic, modports, and an interface passed down through a module.
# Icarus Verilog cannot parse an interface with ports or modports, so the case checks itself.
enzan_add_golden_test(interfaces NO_DIFFERENTIAL)

# Arrays of instances, with connections split across the elements.
enzan_add_golden_test(instance_arrays)

# Automatic variables in blocks of static processes, which Icarus Verilog does not implement.
enzan_add_golden_test(automatic NO_DIFFERENTIAL)

# Immediate assertions. Icarus Verilog does not run a cover's action, so the case checks itself.
enzan_add_golden_test(assertions NO_DIFFERENTIAL)

# Event controls on selects, and event lists inside a body.
enzan_add_golden_test(events)

# Intra-assignment delays, and inertial delays on continuous assignments.
enzan_add_golden_test(delays)

# `wait (condition)`.
enzan_add_golden_test(wait)

# Side effects in operands that `&&`, `||` and `?:` leave unevaluated.
enzan_add_golden_test(short_circuit)

# A type parameter handed down a second level. Icarus Verilog does not parse type parameters.
enzan_add_golden_test(type_parameters NO_DIFFERENTIAL)

# Packages, and package variables shared by every module that uses them.
enzan_add_golden_test(packages)

# Loops: `break`, `continue` and `foreach`. Icarus Verilog 12, the version CI installs, has no
# `break` or `continue`; version 13 has both and agrees. A `break` in a `foreach` of two
# dimensions leaves the whole statement, as Verilator does, where Icarus Verilog 13 leaves only
# the inner dimension.
enzan_add_golden_test(loops NO_DIFFERENTIAL)
enzan_add_golden_test(foreach_break NO_DIFFERENTIAL)

# Constructs a parameter makes degenerate, in code a constant condition rules out, accepted with
# a warning. Icarus Verilog accepts most of them but not those in a `?:` arm or a pattern.
enzan_add_golden_test(dead_code NO_DIFFERENTIAL)

# Ports connected to signals wider or narrower than themselves.
enzan_add_golden_test(port_widths)

# File output, and memories loaded from files the design writes first: compared file by file
# with what Icarus Verilog does, each in an empty directory, so they need it.
if(ENZAN_REFERENCE_SIMULATOR)
  foreach(name files readmem)
    add_test(NAME golden.${name}
             COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/run-isolated.sh" $<TARGET_FILE:enzan>
                     "${ENZAN_REFERENCE_SIMULATOR}" "${CMAKE_CURRENT_SOURCE_DIR}/sv/${name}.sv"
                     "${CMAKE_CURRENT_BINARY_DIR}/isolated/${name}")
    set_tests_properties(golden.${name} PROPERTIES PASS_REGULAR_EXPRESSION "PASS ${name}")
  endforeach()
endif()

# Random numbers: `$random` against Icarus Verilog, and the seed and random stability.
enzan_add_golden_test(random)
add_test(NAME golden.random_stability
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/random_stability.sh" $<TARGET_FILE:enzan>
                 "${CMAKE_CURRENT_SOURCE_DIR}/sv/random_stability.sv"
                 "${CMAKE_CURRENT_BINARY_DIR}/random_stability")
set_tests_properties(golden.random_stability PROPERTIES PASS_REGULAR_EXPRESSION
                                                        "PASS random_stability")

# An error reported while the design runs is the design failing in simulation: status 3.
add_test(NAME golden.runtime_error_status
         COMMAND sh -c "\"$1\" --top tb \"$2\" >/dev/null 2>&1; test $? -eq 3" sh
                 $<TARGET_FILE:enzan> "${CMAKE_CURRENT_SOURCE_DIR}/sv/runtime_error.sv")

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
