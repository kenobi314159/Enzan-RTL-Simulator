# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dependency acquisition.
#
# The policy (DESIGN_GUIDE.md section 11.2) is: system packages first,
# FetchContent as a fallback, and a clear actionable error naming the package
# when neither works. "Clearly recognisable dependencies" requirement means a
# user reading the CMake output knows exactly what to install, so the diagnostic
# text below is a feature and not boilerplate.

include(FetchContent)

# ---------------------------------------------------------------------------
# enzan_mark_includes_system(<target>)
# ---------------------------------------------------------------------------
#
# Re-exports a target's include directories as system includes, so that its headers escape
# Enzan's warning set.
#
# Enzan compiles with a wide warning set and CI turns it into -Werror. A dependency built in
# the same tree exports its includes as ordinary ones, so slang's headers — which use
# C-style casts and shadow their own members, as is their prerogative — would fail our build
# with errors in code we do not own. `add_subdirectory(... SYSTEM)` would be the direct fix
# but needs CMake 3.25, above the 3.24 floor of section 11.1.
function(enzan_mark_includes_system target)
  if(NOT TARGET ${target})
    return()
  endif()

  get_target_property(includeDirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
  if(includeDirs)
    set_property(TARGET ${target} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                                  "${includeDirs}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sources a release's tarball carries
# ---------------------------------------------------------------------------
#
# A release's source tarball carries, in third_party/bundled/, the sources FetchContent would
# otherwise download: slang and what slang fetches in turn (DESIGN_GUIDE.md section 11.6). A
# distribution's builder has no network, so a tarball that downloaded would build on none of
# them. Each directory stands in for its download, so a system package is still preferred where
# one is found, as section 11.2 has it. A checkout has no such directory and downloads as before.
set(_enzan_bundled "${PROJECT_SOURCE_DIR}/third_party/bundled")
if(IS_DIRECTORY "${_enzan_bundled}")
  file(
    GLOB _enzan_bundled_names
    LIST_DIRECTORIES true
    RELATIVE "${_enzan_bundled}"
    "${_enzan_bundled}/*")
  foreach(_name IN LISTS _enzan_bundled_names)
    if(IS_DIRECTORY "${_enzan_bundled}/${_name}")
      string(TOUPPER "${_name}" _upper)
      set(FETCHCONTENT_SOURCE_DIR_${_upper}
          "${_enzan_bundled}/${_name}"
          CACHE PATH "The source of ${_name} the release tarball carries")
      message(STATUS "${_name} is built, if it is not found, from the source the tarball carries")
    endif()
  endforeach()
endif()

# FETCHCONTENT_BASE_DIR is deliberately left at its default, one per build
# directory. Pointing several build trees at a shared base directory would share
# not only the downloaded sources but also the dependency *build* trees, and two
# presets writing into one build tree corrupt each other. Re-cloning slang per
# preset costs a shallow clone; the alternative costs correctness.

# ---------------------------------------------------------------------------
# enzan_dependency_missing
# ---------------------------------------------------------------------------
#
# Aborts with an install hint. Called when a dependency is genuinely
# unobtainable, either because it has no source fallback or because downloading
# was disabled.
function(enzan_dependency_missing)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;PURPOSE;ARCH;DEBIAN;FEDORA;FETCHABLE" "")

  set(hint "")
  if(arg_ARCH)
    string(APPEND hint "    Arch, Manjaro    : sudo pacman -S --needed ${arg_ARCH}\n")
  endif()
  if(arg_DEBIAN)
    string(APPEND hint "    Debian, Ubuntu   : sudo apt install ${arg_DEBIAN}\n")
  endif()
  if(arg_FEDORA)
    string(APPEND hint "    Fedora, RHEL     : sudo dnf install ${arg_FEDORA}\n")
  endif()

  set(fallback "")
  if(arg_FETCHABLE)
    set(fallback
        "  Enzan can also build it from source, but that is currently disabled.\n"
        "  Re-run with -DENZAN_ALLOW_FETCH=ON to download and build it.\n")
  endif()

  message(
    FATAL_ERROR
      "Required dependency not found: ${arg_NAME}\n"
      "  Enzan needs it for: ${arg_PURPOSE}\n"
      "  Install it with one of:\n"
      "${hint}"
      "  If it is installed somewhere unusual, point CMake at it with\n"
      "  -DCMAKE_PREFIX_PATH=<prefix> or -D${arg_NAME}_ROOT=<prefix>.\n"
      "${fallback}"
      "  Dependencies are listed in DESIGN_GUIDE.md section 11.3.")
endfunction()

# ---------------------------------------------------------------------------
# GoogleTest — unit tests (DESIGN_GUIDE.md section 14.1)
# ---------------------------------------------------------------------------

if(ENZAN_BUILD_TESTS)
  find_package(GTest 1.12 QUIET)
  if(NOT GTest_FOUND)
    if(ENZAN_ALLOW_FETCH)
      message(STATUS "GoogleTest not found on the system; building it from source")
      FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
        FIND_PACKAGE_ARGS NAMES GTest)
      # GoogleTest's own install and mock targets are not wanted here.
      set(INSTALL_GTEST OFF)
      set(BUILD_GMOCK ON)
      FetchContent_MakeAvailable(googletest)
    else()
      enzan_dependency_missing(
        NAME GTest
        PURPOSE "the unit test suite; configure with -DENZAN_BUILD_TESTS=OFF to skip it"
        ARCH gtest
        DEBIAN libgtest-dev
        FEDORA gtest-devel
        FETCHABLE TRUE)
    endif()
  else()
    message(STATUS "Using system GoogleTest ${GTest_VERSION}")
  endif()
endif()

# ---------------------------------------------------------------------------
# zlib — FST waveform compression (DESIGN_GUIDE.md section 7.3)
# ---------------------------------------------------------------------------

find_package(ZLIB 1.2.11 QUIET)
if(NOT ZLIB_FOUND)
  enzan_dependency_missing(
    NAME ZLIB
    PURPOSE "compressing FST waveform output"
    ARCH zlib
    DEBIAN zlib1g-dev
    FEDORA zlib-devel)
endif()
message(STATUS "Using zlib ${ZLIB_VERSION_STRING}")

# ---------------------------------------------------------------------------
# flex and zstd — building NVC's analysis (DESIGN_GUIDE.md section 5.2)
# ---------------------------------------------------------------------------

# Needed only by the VHDL frontend, and only to build the part of NVC it uses
# (docs/nvc_integration.md). NVC's lexer is a flex source, so flex runs at build time and
# nothing of it is linked. NVC compresses the analysed units it writes into a library with
# zstd, whose header its file layer includes unconditionally. Neither has a source fallback:
# both are packaged everywhere Enzan builds.
if(ENZAN_ENABLE_VHDL)
  find_package(FLEX QUIET)
  if(NOT FLEX_FOUND)
    enzan_dependency_missing(
      NAME FLEX
      PURPOSE
        "generating NVC's VHDL lexer for the VHDL frontend; configure with -DENZAN_ENABLE_VHDL=OFF to skip it"
      ARCH flex
      DEBIAN flex
      FEDORA flex)
  endif()
  message(STATUS "Using flex ${FLEX_VERSION}")

  find_package(ZSTD 1.4 QUIET)
  if(NOT ZSTD_FOUND)
    enzan_dependency_missing(
      NAME ZSTD
      PURPOSE
        "the analysed VHDL units NVC writes into a library, which it compresses with zstd; configure with -DENZAN_ENABLE_VHDL=OFF to skip the VHDL frontend"
      ARCH zstd
      DEBIAN libzstd-dev
      FEDORA libzstd-devel)
  endif()
  message(STATUS "Using zstd ${ZSTD_VERSION}")
endif()

# ---------------------------------------------------------------------------
# slang — SystemVerilog frontend (DESIGN_GUIDE.md section 5.1)
# ---------------------------------------------------------------------------

# slang is not packaged by any mainstream distribution, so in practice the
# source build below is the path almost every contributor takes. The
# find_package attempt is kept first anyway, both because the policy requires it
# and because distributions may package it later.
set(ENZAN_SLANG_VERSION
    "11.0"
    CACHE STRING "Minimum acceptable slang version")
set(ENZAN_SLANG_GIT_TAG
    "v11.0"
    CACHE STRING "slang revision built when no system package is available")

if(ENZAN_ENABLE_SYSTEMVERILOG)
  find_package(slang ${ENZAN_SLANG_VERSION} QUIET CONFIG)
  if(NOT slang_FOUND)
    # Building the source a release tarball carries downloads nothing.
    if(NOT ENZAN_ALLOW_FETCH AND NOT FETCHCONTENT_SOURCE_DIR_SLANG)
      enzan_dependency_missing(
        NAME slang
        PURPOSE
          "parsing and elaborating SystemVerilog; configure with -DENZAN_ENABLE_SYSTEMVERILOG=OFF to skip it"
        FETCHABLE TRUE)
    endif()

    message(STATUS "slang not found on the system; building ${ENZAN_SLANG_GIT_TAG} from source")

    # slang builds a command-line tool, a test suite and documentation by
    # default when it is the top-level project. As a subproject it only needs
    # to produce the library.
    set(SLANG_INCLUDE_TOOLS OFF)
    set(SLANG_INCLUDE_TESTS OFF)
    set(SLANG_INCLUDE_DOCS OFF)
    set(SLANG_INCLUDE_INSTALL OFF)

    # slang must build its own fmt rather than use a system one.
    #
    # fmt 12.2 as packaged by distributions exports C++20 module support on its
    # imported target. CMake then turns on module dependency scanning for every
    # target that links it, GCC compiles slang's sources with -fmodules-ts, and
    # slang's header-based `fmt::format` calls stop resolving. The failure is
    # upstream of Enzan and reported as `'format' is not a member of 'fmt'`, which
    # gives a contributor no chance of guessing the cause.
    #
    # Disabling the find_package lookup makes slang fall back to the version it
    # pins, which is a small, fast build and removes a variable from ours. Enzan
    # itself uses std::format and never links fmt, so there is no conflict.
    set(CMAKE_DISABLE_FIND_PACKAGE_fmt ON)

    # mimalloc replaces the global allocator, which defeats AddressSanitizer's
    # interceptors and produces both false negatives and spurious reports. The
    # sanitizer job is a correctness gate, so it wins over slang's allocator
    # speed there.
    if(ENZAN_SANITIZERS)
      set(SLANG_USE_MIMALLOC OFF)
      message(STATUS "slang: mimalloc disabled because sanitizers are enabled")
    endif()

    FetchContent_Declare(
      slang
      GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
      GIT_TAG ${ENZAN_SLANG_GIT_TAG}
      GIT_SHALLOW TRUE
      EXCLUDE_FROM_ALL
      FIND_PACKAGE_ARGS ${ENZAN_SLANG_VERSION} CONFIG)

    # slang follows BUILD_SHARED_LIBS, which a distribution's build macros may turn on: Fedora's
    # %cmake does. A shared slang would be a library nothing installs (DESIGN_GUIDE.md section
    # 11.5), and an installed `enzan` would fail to start without it, so it is built static
    # whatever was asked, as every Enzan component is.
    set(_enzan_build_shared_libs ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(slang)
    set(BUILD_SHARED_LIBS ${_enzan_build_shared_libs})

    # slang and the dependencies it brings with it are third-party code; their headers must
    # not be held to Enzan's warning set.
    enzan_mark_includes_system(slang_slang)
    enzan_mark_includes_system(fmt)
    enzan_mark_includes_system(mimalloc)
  else()
    message(STATUS "Using system slang ${slang_VERSION}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tcl — the GUI theme language (DESIGN_GUIDE.md section 10.5)
# ---------------------------------------------------------------------------

# Needed only by the graphical front end, whose theme is a Tcl script.
#
# No version is requested here: CMake's FindTCL module reports no version, so a version
# constraint would make the package unfindable rather than version-checked. The 8.6 requirement
# is checked in the code that embeds the interpreter instead, against TCL_MAJOR_VERSION and
# TCL_MINOR_VERSION from tcl.h.
if(ENZAN_BUILD_GUI)
  find_package(TCL QUIET)
  if(NOT TCL_FOUND)
    enzan_dependency_missing(
      NAME TCL
      PURPOSE
        "the Tcl theme language of the graphical front end; configure with -DENZAN_BUILD_GUI=OFF to skip it"
      ARCH tcl
      DEBIAN tcl-dev
      FEDORA tcl-devel)
  endif()
  message(STATUS "Using Tcl ${TCL_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# Qt 6 — graphical front end (DESIGN_GUIDE.md section 10.1), milestone M2
# ---------------------------------------------------------------------------

if(ENZAN_BUILD_GUI)
  # Qt Test is asked for only when the unit tests are being built: it supplies QSignalSpy,
  # which is how a test waits for a signal that a real socket will deliver whenever it does.
  set(_enzan_qt_components Widgets Gui Core)
  if(ENZAN_BUILD_TESTS)
    list(APPEND _enzan_qt_components Test)
  endif()

  find_package(Qt6 6.4 QUIET COMPONENTS ${_enzan_qt_components})
  if(NOT Qt6_FOUND)
    enzan_dependency_missing(
      NAME Qt6
      PURPOSE "the Qt 6 Widgets graphical front end; configure with -DENZAN_BUILD_GUI=OFF to skip it"
      ARCH qt6-base
      DEBIAN qt6-base-dev
      FEDORA qt6-qtbase-devel)
  endif()
  message(STATUS "Using Qt ${Qt6_VERSION}")
endif()

# ---------------------------------------------------------------------------
# LLVM — JIT backend (DESIGN_GUIDE.md section 6.3), milestone M8
# ---------------------------------------------------------------------------

if(ENZAN_ENABLE_JIT)
  find_package(LLVM 17 QUIET CONFIG)
  if(NOT LLVM_FOUND)
    enzan_dependency_missing(
      NAME LLVM
      PURPOSE "the ORC JIT execution backend"
      ARCH llvm
      DEBIAN llvm-dev
      FEDORA llvm-devel)
  endif()
  message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")
endif()
