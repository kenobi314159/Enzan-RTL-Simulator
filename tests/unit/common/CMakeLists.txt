# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  common
  SOURCES AssertTests.cpp VersionTests.cpp
  DEPENDS enzan::common)
