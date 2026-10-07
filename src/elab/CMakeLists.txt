# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  elab
  SOURCES ElaboratedDesign.cpp Elaborator.cpp Specialiser.cpp
  HEADERS ElaboratedDesign.h Elaborator.h JoinNames.h Specialiser.h
  DEPENDS enzan::common enzan::eir)
