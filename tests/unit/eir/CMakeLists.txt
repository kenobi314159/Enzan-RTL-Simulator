# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  eir
  SOURCES BitOpsTests.cpp
          BitVectorTests.cpp
          DesignTests.cpp
          InstructionTests.cpp
          LibraryTests.cpp
          ModuleTests.cpp
          ParameterExpressionTests.cpp
          ProcessTests.cpp
          SerializationTests.cpp
          SpecialiseTests.cpp
          TypeTests.cpp
          VerifierTests.cpp
  DEPENDS enzan::eir)
