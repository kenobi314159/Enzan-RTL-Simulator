# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-checking VHDL cases (DESIGN_GUIDE.md section 14.2), as ../ has for SystemVerilog.
#
# Every case is a testbench entity `tb` that ends by reporting `PASS <name>` and calling
# `std.env.finish`, so a case that stops early fails on the missing line. Any error fails it too:
# an assertion of severity error is reported and the run goes on, so the line alone would not.

# ---------------------------------------------------------------------------
# enzan_add_vhdl_golden_test(<name>)
# ---------------------------------------------------------------------------
#
# Registers `golden.vhdl.<name>`, which runs <name>.vhd and requires the PASS line, and
# `golden.vhdl.<name>.differential`, which compares the run against GHDL's (section 14.3).
function(enzan_add_vhdl_golden_test name)
  set(source "${CMAKE_CURRENT_SOURCE_DIR}/${name}.vhd")
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "golden test '${name}' has no source at ${source}")
  endif()

  add_test(NAME golden.vhdl.${name} COMMAND enzan --top tb "${source}")
  set_tests_properties(
    golden.vhdl.${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "PASS ${name}"
               FAIL_REGULAR_EXPRESSION ": (error|fatal): "
               TIMEOUT 60)

  if(NOT ENZAN_VHDL_REFERENCE_SIMULATOR)
    return()
  endif()
  add_test(
    NAME golden.vhdl.${name}.differential
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run-ghdl-differential.sh" $<TARGET_FILE:enzan>
            "${ENZAN_VHDL_REFERENCE_SIMULATOR}" "${CMAKE_CURRENT_SOURCE_DIR}/compare-ghdl.py"
            "${source}" tb)
  set_tests_properties(golden.vhdl.${name}.differential PROPERTIES TIMEOUT 120)
endfunction()

# GHDL is M6's reference for VHDL. Skipped rather than failed when it is not installed, as Icarus
# Verilog is for SystemVerilog, so that a contributor without it can still run the suite; CI
# installs it.
find_program(ENZAN_VHDL_REFERENCE_SIMULATOR ghdl)
if(ENZAN_VHDL_REFERENCE_SIMULATOR)
  message(STATUS "VHDL differential tests will compare against ${ENZAN_VHDL_REFERENCE_SIMULATOR}")
else()
  message(STATUS "ghdl not found; VHDL differential tests are skipped")
endif()

# A counter on `bit` and `integer` alone, below a testbench that clocks and resets it.
enzan_add_vhdl_golden_test(counter)

# Sequential statements: loops, case, the arithmetic operators' signs, short-circuit operators,
# waits for time and for conditions, and a clock that stops by reassigning itself.
enzan_add_vhdl_golden_test(statements)

# Arrays of bits and of arrays: aggregates, slices, elements by static and dynamic index, signal
# assignments to parts, concatenation, and the operators on arrays.
enzan_add_vhdl_golden_test(arrays)

# Subprograms, callable and inlined, of a package and of an architecture.
enzan_add_vhdl_golden_test(subprograms)

# A counter on std_logic and numeric_std, which are translated from the IEEE packages' own bodies.
enzan_add_vhdl_golden_test(ieee_counter)

# A FIFO on std_logic and numeric_std: with the counter, the designs of DESIGN_GUIDE.md section 18's
# checkpoint 6b.
enzan_add_vhdl_golden_test(fifo)

# Records: as signals, variables and ports, with fields assigned and aggregates of every kind.
enzan_add_vhdl_golden_test(records)

# Generate statements and blocks, flattened into the architecture they are in, and ports
# associated with parts of signals or in parts.
enzan_add_vhdl_golden_test(generate)

# Report messages built from strings a design computes.
enzan_add_vhdl_golden_test(messages)

# Loops unrolled because a slice in them moves with their parameter.
enzan_add_vhdl_golden_test(unrolled)

# numeric_std's shifts by run-time amounts, which assign slices of run-time lengths.
enzan_add_vhdl_golden_test(shifts)

# Reals: generics, signals, arithmetic, comparisons and conversions.
enzan_add_vhdl_golden_test(reals)

# std.textio, run natively: lines to output and to a file, and read back.
enzan_add_vhdl_golden_test(textio)

# Resolved signals: std_logic's resolution and a design's own, with several drivers.
enzan_add_vhdl_golden_test(resolution)

# GHDL's run options: the IEEE packages' warnings reported, or not, and the severity that ends a
# run. Each runs the same design and is told apart by what it prints.
set(options_source "${CMAKE_CURRENT_SOURCE_DIR}/options.vhd")
add_test(NAME golden.vhdl.options.ieee_asserts_enabled COMMAND enzan --top tb "${options_source}")
set_tests_properties(golden.vhdl.options.ieee_asserts_enabled
                     PROPERTIES PASS_REGULAR_EXPRESSION "metavalue detected.*PASS options")
add_test(NAME golden.vhdl.options.ieee_asserts_disabled
         COMMAND enzan --top tb --ieee-asserts disable "${options_source}")
set_tests_properties(golden.vhdl.options.ieee_asserts_disabled
                     PROPERTIES PASS_REGULAR_EXPRESSION "converted 0" FAIL_REGULAR_EXPRESSION
                                "metavalue")
add_test(NAME golden.vhdl.options.assert_level_error
         COMMAND enzan --top tb --assert-level error "${options_source}")
set_tests_properties(golden.vhdl.options.assert_level_error
                     PROPERTIES PASS_REGULAR_EXPRESSION "an error" FAIL_REGULAR_EXPRESSION
                                "PASS options")

# Components bound by configuration specifications.
enzan_add_vhdl_golden_test(configurations)
