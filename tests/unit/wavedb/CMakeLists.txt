# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  wavedb
  SOURCES FstWriterTests.cpp VcdWriterTests.cpp WaveformWriterTests.cpp
  DEPENDS enzan::wavedb)
