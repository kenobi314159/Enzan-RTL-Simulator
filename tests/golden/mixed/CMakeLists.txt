# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self-checking designs of both languages: a VHDL testbench `tb`, <name>.vhd, around
# SystemVerilog modules, <name>.sv, that ends by reporting `PASS <name>` and calling
# `std.env.finish`, as ../vhdl's cases do; or a SystemVerilog testbench `tb`, <name>.sv, around
# VHDL units, <name>.vhd, that ends by displaying `PASS <name>`.

# ---------------------------------------------------------------------------
# enzan_add_mixed_golden_test(<name>)
# ---------------------------------------------------------------------------
#
# Registers `golden.mixed.<name>`, which runs the design and requires the PASS line, and
# `golden.mixed.<name>.differential`, which compares the run against GHDL's of the same design all
# in VHDL, the modules as <name>_ref.vhd: no free simulator runs both languages.
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

# ---------------------------------------------------------------------------
# enzan_add_mixed_sv_golden_test(<name>)
# ---------------------------------------------------------------------------
#
# Registers `golden.mixed.<name>` for a SystemVerilog testbench, and
# `golden.mixed.<name>.differential`, which compares its transcript with Icarus Verilog's of the
# same design all in SystemVerilog, the units as <name>_ref.sv.
function(enzan_add_mixed_sv_golden_test name)
  set(directory "${CMAKE_CURRENT_SOURCE_DIR}")
  foreach(source "${name}.sv" "${name}.vhd" "${name}_ref.sv")
    if(NOT EXISTS "${directory}/${source}")
      message(FATAL_ERROR "golden test '${name}' has no ${source}")
    endif()
  endforeach()

  add_test(NAME golden.mixed.${name} COMMAND enzan --top tb "${directory}/${name}.sv"
                                             "${directory}/${name}.vhd")
  set_tests_properties(
    golden.mixed.${name}
    PROPERTIES PASS_REGULAR_EXPRESSION "PASS ${name}"
               FAIL_REGULAR_EXPRESSION ": (error|fatal|warning): |\nFAIL"
               TIMEOUT 60)

  if(NOT ENZAN_REFERENCE_SIMULATOR)
    return()
  endif()
  add_test(NAME golden.mixed.${name}.differential
           COMMAND "${directory}/run-sv-differential.sh" $<TARGET_FILE:enzan>
                   "${ENZAN_REFERENCE_SIMULATOR}" "${directory}" ${name})
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

# Parameters declared as vendors' models declare them, set from generics: a boolean meeting a
# string and a bit, a string and an integer.
enzan_add_mixed_golden_test(parameters)

# A pull-up and a pull-down driving VHDL signals beside strong drivers: a value SystemVerilog drives
# weakly crosses as H or L, so a pull holds a signal alone and yields to the driver.
enzan_add_mixed_golden_test(pulls)

# A VHDL counter three times, with its generics' defaults, with a width and a step set, and with a
# boolean cleared; its ports a vector, an integer and a boolean.
enzan_add_mixed_sv_golden_test(generics)

# A record as a packed struct, an array of vectors as an unpacked array, and a bus both sides drive
# and release.
enzan_add_mixed_sv_golden_test(shapes)

# SystemVerilog instantiating VHDL instantiating SystemVerilog, at two widths.
enzan_add_mixed_sv_golden_test(nesting)

# cocotb driving a design of both languages, through the toplevel's language's interface and the
# other's, once with each language's toplevel, when a Python has cocotb.
if(ENZAN_HAVE_COCOTB)
  foreach(language verilog vhdl)
    add_test(NAME golden.mixed.cocotb.${language}
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../run-cocotb-check.sh" $<TARGET_FILE:enzan>
                     "${ENZAN_COCOTB_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/cocotb" ${language})
    set_tests_properties(
      golden.mixed.cocotb.${language}
      PROPERTIES PASS_REGULAR_EXPRESSION "PASS cocotb" FAIL_REGULAR_EXPRESSION "FAIL=[1-9]"
                 TIMEOUT 300)
  endforeach()
endif()
