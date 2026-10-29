# SPDX-License-Identifier: GPL-3.0-or-later

# The revision is captured at configure time. Making the generated header depend
# on .git/HEAD would rebuild the world on every commit for the sake of a string
# nothing but `--version` reads, so the staleness is accepted and documented in
# Version.h instead.
#
# A release tarball records the revision it was made from (DESIGN_GUIDE.md
# section 11.6), and that comes first. A tarball is not a checkout, and one
# unpacked inside a checkout, as a distribution's packaging repository unpacks
# it, would otherwise be given that repository's revision.
set(ENZAN_SOURCE_REVISION "unknown")
find_package(Git QUIET)
if(EXISTS "${PROJECT_SOURCE_DIR}/.tarball-revision")
  file(STRINGS "${PROJECT_SOURCE_DIR}/.tarball-revision" ENZAN_SOURCE_REVISION LIMIT_COUNT 1)
elseif(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=8
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _enzan_git_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _enzan_git_result)
  if(_enzan_git_result EQUAL 0 AND _enzan_git_revision)
    set(ENZAN_SOURCE_REVISION "${_enzan_git_revision}")
  endif()
endif()

if(CMAKE_BUILD_TYPE)
  set(ENZAN_BUILD_TYPE_STRING "${CMAKE_BUILD_TYPE}")
else()
  set(ENZAN_BUILD_TYPE_STRING "unspecified")
endif()

set(_enzan_version_info "${PROJECT_BINARY_DIR}/generated/common/VersionInfo.h")
configure_file(VersionInfo.h.in "${_enzan_version_info}" @ONLY)

# Where the rest of Enzan is from the directory of the executable (cmake/EnzanInstallDirs.cmake).
set(_enzan_installation_info "${PROJECT_BINARY_DIR}/generated/common/InstallationInfo.h")
configure_file(InstallationInfo.h.in "${_enzan_installation_info}" @ONLY)

enzan_add_component(
  common
  SOURCES Assert.cpp Diagnostics.cpp Installation.cpp SourceManager.cpp Version.cpp
  HEADERS Assert.h
          DiagnosticCodes.def
          Diagnostics.h
          Installation.h
          SourceLocation.h
          SourceManager.h
          Version.h
          "${_enzan_version_info}"
          "${_enzan_installation_info}")
