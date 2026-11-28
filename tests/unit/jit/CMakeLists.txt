# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  jit
  SOURCES BackendTests.cpp EngineTests.cpp SessionTests.cpp
  DEPENDS enzan::jit enzan::runtime enzan::session)
target_include_directories(enzan_unittest_jit SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
