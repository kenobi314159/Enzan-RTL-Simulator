# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp
          BitVector.cpp
          Design.cpp
          Instruction.cpp
          Library.cpp
          Module.cpp
          Process.cpp
          Serialization.cpp
          Type.cpp
          Udp.cpp
          UnitRequest.cpp
          Verifier.cpp
  HEADERS ArrayDimension.h
          BitOps.h
          BitVector.h
          Design.h
          Instruction.h
          Library.h
          Module.h
          Opcodes.def
          Process.h
          Serialization.h
          Type.h
          Udp.h
          UnitRequest.h
          Verifier.h
  DEPENDS enzan::common)
