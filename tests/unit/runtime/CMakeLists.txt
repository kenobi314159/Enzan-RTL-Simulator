# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  runtime
  SOURCES ExecutionTests.cpp FormatTests.cpp KernelTests.cpp ReadMemTests.cpp SchedulerTests.cpp
          TimeTests.cpp
  DEPENDS enzan::runtime)
