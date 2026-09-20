# SPDX-License-Identifier: GPL-3.0-or-later
#
# User-facing executables (DESIGN_GUIDE.md section 12).

add_subdirectory(enzan)

if(ENZAN_BUILD_GUI)
  add_subdirectory(enzan-gui) # M2
endif()
