# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  wavedb
  SOURCES VcdWriter.cpp WaveformWriter.cpp
  HEADERS VcdWriter.h WaveformWriter.h
  DEPENDS enzan::common enzan::eir enzan::runtime)
