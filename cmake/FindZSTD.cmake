# SPDX-License-Identifier: GPL-3.0-or-later
#
# Finds libzstd, which NVC compresses its analysed units with (docs/nvc_integration.md).
#
# CMake ships no module for it. zstd's own CMake package is installed by some distributions and
# not others, depending on how they built zstd, and where it is it names its targets
# zstd::libzstd_shared and zstd::libzstd_static rather than one target. Looking for the header
# and the library directly works for every packaging; this reads the version from the header and
# honours ZSTD_ROOT as any find_package does.
#
# Provides ZSTD_FOUND, ZSTD_VERSION and the imported target ZSTD::ZSTD.

find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY zstd)

if(ZSTD_INCLUDE_DIR AND EXISTS "${ZSTD_INCLUDE_DIR}/zstd.h")
  file(STRINGS "${ZSTD_INCLUDE_DIR}/zstd.h" _zstd_version_lines
       REGEX "^#define ZSTD_VERSION_(MAJOR|MINOR|RELEASE) +[0-9]+")
  foreach(_part MAJOR MINOR RELEASE)
    string(REGEX MATCH "ZSTD_VERSION_${_part} +([0-9]+)" _ "${_zstd_version_lines}")
    set(_zstd_${_part} "${CMAKE_MATCH_1}")
  endforeach()
  set(ZSTD_VERSION "${_zstd_MAJOR}.${_zstd_MINOR}.${_zstd_RELEASE}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  ZSTD
  REQUIRED_VARS ZSTD_LIBRARY ZSTD_INCLUDE_DIR
  VERSION_VAR ZSTD_VERSION)

if(ZSTD_FOUND AND NOT TARGET ZSTD::ZSTD)
  add_library(ZSTD::ZSTD UNKNOWN IMPORTED)
  set_target_properties(ZSTD::ZSTD PROPERTIES IMPORTED_LOCATION "${ZSTD_LIBRARY}"
                                              INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
endif()

mark_as_advanced(ZSTD_INCLUDE_DIR ZSTD_LIBRARY)
