# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pipeline one `enzan` process holds: analysis, elaboration, the kernel and the waveform
# files. Owns a kernel, so the GUI must not link it (DESIGN_GUIDE.md section 2.2).

enzan_add_component(
  session
  SOURCES Session.cpp
  HEADERS Session.h
  DEPENDS enzan::common enzan::eir enzan::elab enzan::runtime enzan::wavedb
  PRIVATE_DEPENDS enzan::codegen)

if(ENZAN_ENABLE_SYSTEMVERILOG)
  target_link_libraries(enzan_session PRIVATE enzan::frontend_sv)
endif()
