# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  eir
  SOURCES BitOpsTests.cpp
          BitVectorTests.cpp
          DesignTests.cpp
          InstructionTests.cpp
          LibraryTests.cpp
          ModuleTests.cpp
          ProcessTests.cpp
          SerializationTests.cpp
          TypeTests.cpp
          UdpTests.cpp
          VerifierTests.cpp
  DEPENDS enzan::eir)
