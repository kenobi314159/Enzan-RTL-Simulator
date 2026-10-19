# SPDX-License-Identifier: GPL-3.0-or-later

if(ENZAN_ENABLE_SYSTEMVERILOG)
  add_subdirectory(sv)
endif()

if(ENZAN_ENABLE_VHDL)
  add_subdirectory(vhdl)
endif()
