# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pipeline one `enzan` process holds: analysis, elaboration, the kernel and the waveform
# files. Owns a kernel, so the GUI must not link it.

enzan_add_component(
  session
  SOURCES FileList.cpp Session.cpp
  HEADERS FileList.h Session.h
  DEPENDS enzan::common enzan::eir enzan::elab enzan::runtime enzan::wavedb
  PRIVATE_DEPENDS enzan::codegen enzan::pli)

if(ENZAN_ENABLE_SYSTEMVERILOG)
  target_link_libraries(enzan_session PRIVATE enzan::frontend_sv)
endif()
if(ENZAN_ENABLE_VHDL)
  target_link_libraries(enzan_session PRIVATE enzan::frontend_vhdl)
endif()
