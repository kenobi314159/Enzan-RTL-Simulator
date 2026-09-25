# SPDX-License-Identifier: GPL-3.0-or-later
#
# User-facing executables (DESIGN_GUIDE.md section 12).

add_subdirectory(enzan)

if(ENZAN_BUILD_GUI)
  # Fail with the reason rather than with "no CMakeLists.txt in that directory",
  # which tells a user nothing about why.
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/enzan-gui/CMakeLists.txt")
    message(
      FATAL_ERROR
        "ENZAN_BUILD_GUI is ON, but the graphical front end does not exist yet.\n"
        "It arrives with milestone M2 (DESIGN_GUIDE.md section 18). Configure with\n"
        "-DENZAN_BUILD_GUI=OFF.")
  endif()
  add_subdirectory(enzan-gui)
endif()
