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
# Registers `golden.vhdl.<name>`, which runs <name>.vhd and requires the PASS line.
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
endfunction()

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
