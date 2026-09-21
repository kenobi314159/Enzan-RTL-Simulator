# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  ipc
  SOURCES KernelServerTests.cpp ProtocolTests.cpp TransportTests.cpp
  DEPENDS enzan::ipc enzan::ipc_server)
