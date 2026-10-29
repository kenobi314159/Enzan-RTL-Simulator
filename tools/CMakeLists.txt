# SPDX-License-Identifier: GPL-3.0-or-later
#
# User-facing executables, and the build-time tool that builds VHDL's standard libraries
# for them.

add_subdirectory(enzan)

# cocotb's simulator makefile, and the script that adds it to a copy of cocotb's makefiles. Each
# finds the other beside itself, and `enzan` on the PATH or in ENZAN_BIN_DIR, so an installation
# needs nothing of the source tree. A cocotb testbench needs a frontend.
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
  add_subdirectory(enzan-gui)
endif()
