# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  eir
  SOURCES BitOps.cpp
          BitVector.cpp
          Design.cpp
          Instruction.cpp
          Library.cpp
          Module.cpp
          ParameterExpression.cpp
          Process.cpp
          Serialization.cpp
          Type.cpp
          Verifier.cpp
  HEADERS BitOps.h
          BitVector.h
          Design.h
          Instruction.h
          Library.h
          Module.h
          Opcodes.def
          ParameterExpression.h
          Process.h
          Serialization.h
          Type.h
          Verifier.h
  DEPENDS enzan::common)
