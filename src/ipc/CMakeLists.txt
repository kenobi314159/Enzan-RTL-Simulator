# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  ipc
  SOURCES Codec.cpp Protocol.cpp Transport.cpp
  HEADERS Codec.h Protocol.h Transport.h
  DEPENDS enzan::common enzan::eir)
