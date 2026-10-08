# SPDX-License-Identifier: GPL-3.0-or-later
#
# Header-only helpers shared by the unit tests. Every test binary links this, so a helper that
# three fixtures had each written for themselves has one home instead.

add_library(enzan_test_support INTERFACE)
add_library(enzan::test_support ALIAS enzan_test_support)

# The parent directory, so that an include reads "support/Literals.h" and says where it comes
# from rather than looking like a header of the component under test.
target_include_directories(enzan_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")

target_link_libraries(enzan_test_support INTERFACE enzan::eir GTest::gtest)

# ---------------------------------------------------------------------------
# VPI applications for the tests that load one
# ---------------------------------------------------------------------------
#
# Built as the shared libraries `--vpi` loads, so that what is tested is loading rather than a
# stand-in for it.
foreach(plugin IN ITEMS StartupPlugin SilentPlugin)
  add_library(enzan_test_${plugin} MODULE plugins/${plugin}.cpp)
  target_link_libraries(enzan_test_${plugin} PRIVATE enzan::compile_options enzan::vpi_headers)
endforeach()

# enzan_use_test_plugins(<target>)
#
# Lets a test executable load the plugins: it exports the routines they resolve against, as
# `enzan` does, depends on them so that they are built first, and is told where they are.
function(enzan_use_test_plugins target)
  enzan_export_vpi(${target})
  add_dependencies(${target} enzan_test_StartupPlugin enzan_test_SilentPlugin)
  target_compile_definitions(
    ${target}
    PRIVATE ENZAN_TEST_STARTUP_PLUGIN="$<TARGET_FILE:enzan_test_StartupPlugin>"
            ENZAN_TEST_SILENT_PLUGIN="$<TARGET_FILE:enzan_test_SilentPlugin>")
endfunction()
