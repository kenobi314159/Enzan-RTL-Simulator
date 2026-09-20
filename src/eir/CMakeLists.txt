# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp
          BitVector.cpp
          Design.cpp
          Instruction.cpp
          Module.cpp
          Process.cpp
          Type.cpp
          Verifier.cpp
  HEADERS BitOps.h
          BitVector.h
          Design.h
          Instruction.h
          Module.h
          Opcodes.def
          Process.h
          Type.h
          Verifier.h
  DEPENDS enzan::common)
