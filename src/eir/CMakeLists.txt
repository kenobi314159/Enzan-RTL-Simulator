# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp BitVector.cpp
  HEADERS BitOps.h BitVector.h
  DEPENDS enzan::common)
