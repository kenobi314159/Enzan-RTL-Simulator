# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-checking designs of both languages (DESIGN_GUIDE.md sections 14.2 and 18, M7): a VHDL
# testbench `tb`, <name>.vhd, around SystemVerilog modules, <name>.sv, that ends by reporting
# `PASS <name>` and calling `std.env.finish`, as ../vhdl's cases do.

# ---------------------------------------------------------------------------
# enzan_add_mixed_golden_test(<name>)
# ---------------------------------------------------------------------------
#
# Registers `golden.mixed.<name>`, which runs the design and requires the PASS line, and
# `golden.mixed.<name>.differential`, which compares the run against GHDL's of the same design all
# in VHDL, the modules as <name>_ref.vhd (section 14.3): no free simulator runs both languages.
function(enzan_add_mixed_golden_test name)
  set(directory "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(source "${name}.vhd" "${name}.sv" "${name}_ref.vhd")
    if(NOT EXISTS "${directory}/${source}")
      message(FATAL_ERROR "golden test '${name}' has no ${source}")
    endif()
  endforeach()

  add_test(NAME golden.mixed.${name} COMMAND enzan --top tb "${directory}/${name}.vhd"
                                             "${directory}/${name}.sv")
  set_tests_properties(
    golden.mixed.${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "PASS ${name}"
               FAIL_REGULAR_EXPRESSION ": (error|fatal|warning): "
               TIMEOUT 60)

  if(NOT ENZAN_VHDL_REFERENCE_SIMULATOR)
    return()
  endif()
  add_test(
    NAME golden.mixed.${name}.differential
    COMMAND "${directory}/run-differential.sh" $<TARGET_FILE:enzan>
            "${ENZAN_VHDL_REFERENCE_SIMULATOR}" "${directory}/../vhdl/compare-ghdl.py"
            "${directory}" ${name})
  set_tests_properties(golden.mixed.${name}.differential PROPERTIES TIMEOUT 120)
endfunction()

# Two counters, of widths generics set through a parameter with no default, one with a limit a
# generic sets and one with the parameter's default.
enzan_add_mixed_golden_test(counter)

# A port of each kind: std_logic and its vectors each way, with every one of the nine values
# crossing, an integer, a bit and a boolean, and a bus both sides drive and release.
enzan_add_mixed_golden_test(ports)

# A module holding two others, connected through slices each way, a constant and an open output.
enzan_add_mixed_golden_test(pipeline)
