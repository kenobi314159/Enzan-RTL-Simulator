# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  ipc
  SOURCES ProtocolTests.cpp TransportTests.cpp
  DEPENDS enzan::ipc)
