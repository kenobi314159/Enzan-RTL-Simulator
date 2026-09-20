# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  eir
  SOURCES BitOpsTests.cpp BitVectorTests.cpp
  DEPENDS enzan::eir)
