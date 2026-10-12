# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  frontend_vhdl
  SOURCES NvcAnalysis.cpp VhdlFrontend.cpp
  HEADERS NvcAnalysis.h NvcApi.h VhdlFrontend.h
  DEPENDS enzan::common enzan::eir
  PRIVATE_DEPENDS enzan::nvc)
