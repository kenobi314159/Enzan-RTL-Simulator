# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  control
  SOURCES FileListTests.cpp InterpreterTests.cpp
  DEPENDS enzan::control)
