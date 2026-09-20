# SPDX-License-Identifier: GPL-3.0-or-later

if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_subdirectory(sv)
endif()

if(ENZAN_ENABLE_VHDL)
  # Fail with the reason rather than with "no CMakeLists.txt in that directory".
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vhdl/CMakeLists.txt")
    message(
      FATAL_ERROR
        "ENZAN_ENABLE_VHDL is ON, but the VHDL frontend does not exist yet.\n"
        "It arrives with milestone M6 (DESIGN_GUIDE.md section 18). Configure with\n"
        "-DENZAN_ENABLE_VHDL=OFF.")
  endif()
  add_subdirectory(vhdl)
endif()
