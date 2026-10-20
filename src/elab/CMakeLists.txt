# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  elab
  SOURCES Boundary.cpp ElaboratedDesign.cpp Elaborator.cpp
  HEADERS Boundary.h ElaboratedDesign.h Elaborator.h
  DEPENDS enzan::common enzan::eir)
