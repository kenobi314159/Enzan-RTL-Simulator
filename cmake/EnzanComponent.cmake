# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers for declaring the components of DESIGN_GUIDE.md section 12 and their
# unit tests. Every component is declared the same way so that the build files
# stay uniform and so that project-wide rules can be enforced in one place.

# ---------------------------------------------------------------------------
# enzan_add_component(<name> SOURCES <...> [HEADERS <...>]
#                            [DEPENDS <...>] [PRIVATE_DEPENDS <...>])
# ---------------------------------------------------------------------------
#
# Declares the static library for one component of src/, named `enzan_<name>`
# with the alias `enzan::<name>`.
#
# DEPENDS lists other components and external targets whose headers appear in
# this component's own headers; PRIVATE_DEPENDS lists those used only in its
# sources. Keeping the distinction means a component's public dependency surface
# is visible in the build file, which is what makes the "allowed dependencies"
# statement in each component README auditable rather than aspirational.
function(enzan_add_component name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;HEADERS;DEPENDS;PRIVATE_DEPENDS")

  if(NOT arg_SOURCES)
    message(FATAL_ERROR "enzan_add_component(${name}): no SOURCES given")
  endif()

  # DESIGN_GUIDE.md section 12 requires a README per component. Enforcing it
  # here rather than in review is the only way it stays true.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    message(
      FATAL_ERROR
        "Component '${name}' has no README.md.\n"
        "DESIGN_GUIDE.md section 12 requires each src/ component to document\n"
        "its responsibility and its allowed dependencies on other components.\n"
        "Expected at: ${CMAKE_CURRENT_SOURCE_DIR}/README.md")
  endif()

  add_library(enzan_${name} STATIC ${arg_SOURCES} ${arg_HEADERS})
  add_library(enzan::${name} ALIAS enzan_${name})

  target_link_libraries(enzan_${name} PUBLIC enzan::compile_options ${arg_DEPENDS})
  if(arg_PRIVATE_DEPENDS)
    target_link_libraries(enzan_${name} PRIVATE ${arg_PRIVATE_DEPENDS})
  endif()

  set_target_properties(enzan_${name} PROPERTIES OUTPUT_NAME "enzan_${name}"
                                                 POSITION_INDEPENDENT_CODE ON)
endfunction()

# ---------------------------------------------------------------------------
# enzan_add_unit_test(<name> SOURCES <...> [DEPENDS <...>])
# ---------------------------------------------------------------------------
#
# Declares one GoogleTest binary under tests/unit/. `name` is the component path
# with slashes replaced by underscores, so that tests/unit/common/ produces
# `enzan_unittest_common` and the mirroring required by section 12 is visible in
# the target list.
#
# Tests are registered individually with gtest_discover_tests so that CI reports
# which case failed rather than only which binary did.
function(enzan_add_unit_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")

  if(NOT arg_SOURCES)
    message(FATAL_ERROR "enzan_add_unit_test(${name}): no SOURCES given")
  endif()

  set(target enzan_unittest_${name})
  add_executable(${target} ${arg_SOURCES})
  target_link_libraries(${target} PRIVATE enzan::compile_options GTest::gtest
                                          GTest::gtest_main ${arg_DEPENDS})

  # A test binary that takes more than a few seconds is a test binary nobody
  # runs; the timeout keeps a hung scheduler from stalling CI indefinitely.
  gtest_discover_tests(
    ${target}
    TEST_PREFIX "unit.${name}."
    PROPERTIES TIMEOUT 120
    DISCOVERY_MODE PRE_TEST)
endfunction()
