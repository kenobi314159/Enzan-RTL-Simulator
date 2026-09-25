# SPDX-License-Identifier: GPL-3.0-or-later
#
# Enzan RTL Simulator — top-level build.
#
# See DESIGN_GUIDE.md section 11 for the toolchain and dependency policy, and
# section 12 for the repository layout this file mirrors.

# 3.24 is the floor because CMakePresets schema version 5 and the
# FIND_PACKAGE_ARGS form of FetchContent_Declare both require it, and the
# dependency policy in DESIGN_GUIDE.md section 11.2 is built on the latter.
cmake_minimum_required(VERSION 3.24)

project(
  Enzan
  VERSION 0.1.0
  DESCRIPTION "Event-driven mixed-language RTL simulator for SystemVerilog and VHDL"
  HOMEPAGE_URL "https://github.com/kenobi314159/Enzan-RTL-Simulator"
  # Enzan itself is C++ only. C is declared because mimalloc, which slang pulls
  # in, is a C project, and because the presets select a matching C compiler for
  # it; declaring the language conditionally would leave that cache variable
  # unused and warned about whenever the frontend is switched off.
  LANGUAGES CXX C)

# Appended to the version in `enzan --version`. Release builds clear it.
set(ENZAN_VERSION_SUFFIX
    "dev"
    CACHE STRING "Pre-release suffix appended to the reported version")

if(ENZAN_VERSION_SUFFIX)
  set(ENZAN_VERSION_STRING "${PROJECT_VERSION}-${ENZAN_VERSION_SUFFIX}")
else()
  set(ENZAN_VERSION_STRING "${PROJECT_VERSION}")
endif()

# ---------------------------------------------------------------------------
# Environment guards
# ---------------------------------------------------------------------------

# Catch the mistake early and with an explanation, rather than letting it
# scatter generated files through the source tree.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
      "Enzan does not support in-source builds.\n"
      "Remove the CMakeCache.txt and CMakeFiles/ this attempt created, then\n"
      "configure with a preset instead, for example:\n"
      "  cmake --preset gcc-debug")
endif()

# Linux only, by explicit design decision (DESIGN_GUIDE.md section 1.3). The
# guard exists so that a port attempt fails with the reason rather than with a
# thousand compile errors.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(
    FATAL_ERROR
      "Enzan targets Linux only; CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'.\n"
      "Windows and macOS support are explicit non-goals (DESIGN_GUIDE.md\n"
      "section 1.3). Changing that is a design decision, not a build fix.")
endif()

# ---------------------------------------------------------------------------
# Language level
# ---------------------------------------------------------------------------

# C++20 is a hard floor: slang requires it (DESIGN_GUIDE.md section 5.1).
# Extensions are off so that a GCC-only construct fails on GCC too.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy and editors both need this, and CI depends on it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Nothing in Enzan or in its dependencies uses C++20 modules, so the module dependency scan
# CMake enables by default for C++20 targets is pure build-time cost. It is also actively
# harmful here: the scan generates dyndep files, and reconfiguring a build directory — switching
# a component on or off, which the presets encourage — leaves ninja 1.13 asserting inside
# Plan::RefreshDyndepDependents and refusing to build at all. Turning the scan off removes both
# the cost and the failure, and must be revisited if Enzan ever adopts modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Keep binaries and libraries in one predictable place per build directory, so
# that scripts and documentation can name a stable path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# Components that belong to later milestones are off by default so that a
# contributor at the current milestone is not asked to install dependencies
# nothing yet uses. The milestone each one belongs to is named here because
# turning one on before its milestone will simply find nothing to build.
option(ENZAN_BUILD_TESTS "Build the unit test suites" ON)
option(ENZAN_ENABLE_SYSTEMVERILOG "Build the slang-based SystemVerilog frontend (M1)" ON)
option(ENZAN_ENABLE_VHDL "Build the NVC-based VHDL frontend (M6)" OFF)
option(ENZAN_ENABLE_TCL "Build the embedded Tcl control layer (M3)" OFF)
option(ENZAN_BUILD_GUI "Build the Qt 6 graphical front end (M2)" OFF)
option(ENZAN_ENABLE_JIT "Build the LLVM JIT backend (M8)" OFF)

option(ENZAN_WERROR "Treat compiler warnings as errors" OFF)
option(ENZAN_ALLOW_FETCH
       "Allow missing dependencies to be downloaded and built from source" ON)

# Semicolon-separated list, for example -DENZAN_SANITIZERS=address;undefined.
set(ENZAN_SANITIZERS
    ""
    CACHE STRING "Sanitizers to enable (address, undefined, thread, ...)")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(EnzanCompilerOptions)
include(EnzanDependencies)
include(EnzanComponent)

# enable_testing() must precede every directory that calls add_test, or CMake
# generates no CTestTestfile.cmake there and those tests vanish without a word.
# tools/ registers the command-line tests, so this cannot move below it.
if(ENZAN_BUILD_TESTS)
  enable_testing()
endif()

# Vendored dependencies come first: components link against them.
add_subdirectory(third_party)

add_subdirectory(src)
add_subdirectory(tools)

if(ENZAN_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

# A target rather than a test. Section 15.3 puts benchmarks in a nightly job that publishes
# cycles/sec, and a measurement that runs as part of the ordinary test suite would be both slow
# and meaningless: a number taken on a loaded machine alongside three hundred other tests is
# not comparable with anything.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_custom_target(
    enzan-bench
    COMMAND "${PROJECT_SOURCE_DIR}/bench/run-bench.sh" "$<TARGET_FILE:enzan>"
    DEPENDS enzan
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Measuring simulated cycles per second"
    USES_TERMINAL)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "Enzan ${ENZAN_VERSION_STRING} configured:")
message(STATUS "  Build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler ........... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Warnings as errors . ${ENZAN_WERROR}")
message(STATUS "  Sanitizers ......... ${ENZAN_SANITIZERS}")
message(STATUS "  Unit tests ......... ${ENZAN_BUILD_TESTS}")
message(STATUS "  SystemVerilog ...... ${ENZAN_ENABLE_SYSTEMVERILOG}")
message(STATUS "  VHDL ............... ${ENZAN_ENABLE_VHDL}")
message(STATUS "  Tcl control layer .. ${ENZAN_ENABLE_TCL}")
message(STATUS "  GUI ................ ${ENZAN_BUILD_GUI}")
message(STATUS "  JIT ................ ${ENZAN_ENABLE_JIT}")
message(STATUS "")
