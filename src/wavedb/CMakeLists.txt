# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  wavedb
  SOURCES VcdWriter.cpp
  HEADERS VcdWriter.h
  DEPENDS enzan::common enzan::eir enzan::runtime)
