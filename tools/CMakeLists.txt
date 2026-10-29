# SPDX-License-Identifier: GPL-3.0-or-later
#
# User-facing executables (DESIGN_GUIDE.md section 12), and the build-time tool that builds
# VHDL's standard libraries for them.

add_subdirectory(enzan)

# cocotb's simulator makefile, and the script that adds it to a copy of cocotb's makefiles
# (section 9.6). Each finds the other beside itself, and `enzan` on the PATH or in ENZAN_BIN_DIR,
# so an installation needs nothing of the source tree. A cocotb testbench needs a frontend.
if(ENZAN_ENABLE_SYSTEMVERILOG OR ENZAN_ENABLE_VHDL)
  install(
    FILES cocotb/Makefile.enzan
    DESTINATION "${ENZAN_INSTALL_DATADIR}/cocotb"
    COMPONENT enzan)
  install(
    PROGRAMS cocotb/makefiles.sh
    DESTINATION "${ENZAN_INSTALL_DATADIR}/cocotb"
    COMPONENT enzan)
endif()

if(ENZAN_ENABLE_VHDL)
  add_subdirectory(vhdl-libraries)
endif()

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
