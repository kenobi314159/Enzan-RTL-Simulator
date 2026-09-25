# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  runtime
  SOURCES FormatTests.cpp KernelTests.cpp SchedulerTests.cpp
  DEPENDS enzan::runtime)
