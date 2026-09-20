# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp BitVector.cpp Instruction.cpp Type.cpp
  HEADERS BitOps.h BitVector.h Instruction.h Opcodes.def Type.h
  DEPENDS enzan::common)
