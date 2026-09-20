# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  elab
  SOURCES ElaboratedDesign.cpp Elaborator.cpp
  HEADERS ElaboratedDesign.h Elaborator.h
  DEPENDS enzan::common enzan::eir)
