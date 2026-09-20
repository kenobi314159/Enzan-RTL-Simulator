# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp BitVector.cpp Type.cpp
  HEADERS BitOps.h BitVector.h Type.h
  DEPENDS enzan::common)
