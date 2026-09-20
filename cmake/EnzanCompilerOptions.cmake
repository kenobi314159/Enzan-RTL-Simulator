# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiler configuration, exposed as a single INTERFACE target that every Enzan
# target links. Keeping the flags in one target rather than in
# `add_compile_options` means third-party code built in the same tree (slang and
# its dependencies) is not subjected to Enzan's warning set, which would
# otherwise make -Werror unusable.

# ---------------------------------------------------------------------------
# Compiler version floor
# ---------------------------------------------------------------------------

# DESIGN_GUIDE.md section 11.1. The floor is set by the C++20 library features
# slang and Enzan both rely on; older compilers fail in confusing ways during
# the build, so they are rejected here with the reason.
set(_enzan_min_gcc 13)
set(_enzan_min_clang 17)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _enzan_min_gcc)
    message(
      FATAL_ERROR
        "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old; Enzan requires GCC "
        "${_enzan_min_gcc} or later (DESIGN_GUIDE.md section 11.1).\n"
        "Select a newer compiler with -DCMAKE_CXX_COMPILER=g++-${_enzan_min_gcc} "
        "or use the clang presets.")
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _enzan_min_clang)
    message(
      FATAL_ERROR
        "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old; Enzan requires Clang "
        "${_enzan_min_clang} or later (DESIGN_GUIDE.md section 11.1).\n"
        "Select a newer compiler with -DCMAKE_CXX_COMPILER=clang++-${_enzan_min_clang} "
        "or use the gcc presets.")
  endif()
else()
  message(
    WARNING
      "Enzan is tested with GCC and Clang only; '${CMAKE_CXX_COMPILER_ID}' is "
      "untested and may fail to build.")
endif()

# ---------------------------------------------------------------------------
# The shared options target
# ---------------------------------------------------------------------------

add_library(enzan_compile_options INTERFACE)
add_library(enzan::compile_options ALIAS enzan_compile_options)

target_compile_features(enzan_compile_options INTERFACE cxx_std_20)

# Component headers are included by their component-qualified path, for example
# `#include "common/Diagnostics.h"`. Headers live beside their sources
# (DESIGN_GUIDE.md section 12), and qualifying the include makes a component
# boundary crossing visible at the top of every file.
target_include_directories(enzan_compile_options INTERFACE "${PROJECT_SOURCE_DIR}/src")

# Headers generated at configure time (common/VersionInfo.h) are laid out under
# this directory with the same component-qualified paths, so that a generated
# header is included exactly like a checked-in one.
target_include_directories(enzan_compile_options INTERFACE "${PROJECT_BINARY_DIR}/generated")

# The warning set is deliberately wide. The kernel manipulates bit vectors,
# widths and offsets constantly, so -Wconversion and -Wsign-conversion are
# carrying real weight here rather than being pedantry: a silent narrowing in
# the value engine is exactly the class of defect that produces plausible but
# wrong simulation results (DESIGN_GUIDE.md section 1.2).
set(_enzan_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-align
    -Wcast-qual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wunused)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(APPEND _enzan_warnings -Wduplicated-cond -Wduplicated-branches
       -Wlogical-op -Wuseless-cast)
endif()

target_compile_options(enzan_compile_options INTERFACE ${_enzan_warnings})

if(ENZAN_WERROR)
  target_compile_options(enzan_compile_options INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# Feature macros
# ---------------------------------------------------------------------------

# One macro per optional component, so that code and `enzan --version` agree on
# what was built without either having to consult the other.
if(ENZAN_ENABLE_SYSTEMVERILOG)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_WITH_SYSTEMVERILOG)
endif()
if(ENZAN_ENABLE_VHDL)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_WITH_VHDL)
endif()
if(ENZAN_ENABLE_TCL)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_WITH_TCL)
endif()
if(ENZAN_ENABLE_JIT)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_WITH_JIT)
endif()
if(ENZAN_SANITIZERS)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_WITH_SANITIZERS)
endif()

# Internal assertions (see src/common/Assert.h) are on everywhere except a plain
# Release build. RelWithDebInfo keeps them because it is the configuration used
# for profiling and for the sanitizer job, where catching a violated invariant
# matters more than the last few percent of speed.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR ENZAN_SANITIZERS)
  target_compile_definitions(enzan_compile_options INTERFACE ENZAN_ENABLE_ASSERTIONS)
endif()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------

# The ASan+UBSan configuration is a CI job, not a build type (DESIGN_GUIDE.md
# section 11.4), so it is expressed as flags on the options target and selected
# by a preset. -fno-sanitize-recover makes a UBSan finding fail the test rather
# than print a note that scrolls past in a green CI log.
if(ENZAN_SANITIZERS)
  list(JOIN ENZAN_SANITIZERS "," _enzan_sanitizer_list)
  set(_enzan_sanitizer_flags -fsanitize=${_enzan_sanitizer_list}
                             -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_compile_options(enzan_compile_options INTERFACE ${_enzan_sanitizer_flags})
  target_link_options(enzan_compile_options INTERFACE ${_enzan_sanitizer_flags})
  message(STATUS "Sanitizers enabled: ${_enzan_sanitizer_list}")
endif()

# ---------------------------------------------------------------------------
# Compiler cache
# ---------------------------------------------------------------------------

# DESIGN_GUIDE.md section 11.1: supported when present, never required.
find_program(ENZAN_CCACHE_EXECUTABLE ccache)
if(ENZAN_CCACHE_EXECUTABLE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${ENZAN_CCACHE_EXECUTABLE}" CACHE STRING "" FORCE)
  message(STATUS "Using ccache: ${ENZAN_CCACHE_EXECUTABLE}")
endif()
