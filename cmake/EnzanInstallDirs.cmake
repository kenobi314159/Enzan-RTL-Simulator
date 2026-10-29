# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where an installation puts each part of Enzan, and the build tree laid out the same way
# (DESIGN_GUIDE.md section 11.5).
#
# `enzan` finds the rest of Enzan from where it is itself, so an installation can be moved, or
# kept beside another version of itself, and still work. The paths it follows are therefore
# relative ones, from the directory of the executable, and they are fixed here, once, for the
# installation and the build tree alike: a build directory is a prefix that has not been
# installed, and one lookup serves both.
#
# Every directory is held relative to the prefix, whatever GNUInstallDirs was given. A
# distribution's build macros may pass absolute ones, and an absolute DESTINATION is one that
# `cmake --install --prefix` and a moved installation would both leave behind.

include(GNUInstallDirs)

# Sets `out` to `directory` relative to the installation prefix, refusing one outside it.
function(_enzan_inside_prefix out directory)
  if(IS_ABSOLUTE "${directory}")
    file(RELATIVE_PATH directory "${CMAKE_INSTALL_PREFIX}" "${directory}")
  endif()
  if(directory MATCHES "^\\.\\./" OR directory STREQUAL ".." OR IS_ABSOLUTE "${directory}")
    message(
      FATAL_ERROR
        "Enzan installs everything inside its prefix, '${CMAKE_INSTALL_PREFIX}', and this\n"
        "directory is outside it: '${directory}'. The executable finds the rest of Enzan\n"
        "relative to itself (DESIGN_GUIDE.md section 11.5), which a directory elsewhere\n"
        "would defeat.")
  endif()
  set(${out}
      "${directory}"
      PARENT_SCOPE)
endfunction()

_enzan_inside_prefix(ENZAN_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
_enzan_inside_prefix(_enzan_libexecdir "${CMAKE_INSTALL_LIBEXECDIR}")
_enzan_inside_prefix(_enzan_datadir "${CMAKE_INSTALL_DATADIR}")
_enzan_inside_prefix(_enzan_includedir "${CMAKE_INSTALL_INCLUDEDIR}")

# The front end is a program only `enzan` starts (section 8.4), so it is kept off the PATH.
set(ENZAN_INSTALL_LIBEXECDIR "${_enzan_libexecdir}/enzan")

# lib/enzan rather than CMAKE_INSTALL_LIBDIR. VHDL's standard libraries are compiled files only
# Enzan reads, and LIBDIR is the directory of libraries other programs link: lib/x86_64-linux-gnu
# on Debian, lib64 on Fedora. Both keep a package's private files in lib/<package>.
set(ENZAN_INSTALL_PKGLIBDIR "lib/enzan")
set(ENZAN_INSTALL_VHDLDIR "${ENZAN_INSTALL_PKGLIBDIR}/vhdl")

set(ENZAN_INSTALL_DATADIR "${_enzan_datadir}/enzan")
set(ENZAN_INSTALL_INCLUDEDIR "${_enzan_includedir}/enzan")

# What `enzan` follows from its own directory, compiled into common/InstallationInfo.h.
file(RELATIVE_PATH ENZAN_FRONT_END_FROM_PROGRAM "/${ENZAN_INSTALL_BINDIR}"
     "/${ENZAN_INSTALL_LIBEXECDIR}/enzan-gui")
file(RELATIVE_PATH ENZAN_VHDL_LIBRARIES_FROM_PROGRAM "/${ENZAN_INSTALL_BINDIR}"
     "/${ENZAN_INSTALL_VHDLDIR}")

# The build tree's copies of the same directories.
set(ENZAN_BUILD_BINDIR "${PROJECT_BINARY_DIR}/${ENZAN_INSTALL_BINDIR}")
set(ENZAN_BUILD_LIBEXECDIR "${PROJECT_BINARY_DIR}/${ENZAN_INSTALL_LIBEXECDIR}")
set(ENZAN_BUILD_VHDLDIR "${PROJECT_BINARY_DIR}/${ENZAN_INSTALL_VHDLDIR}")
