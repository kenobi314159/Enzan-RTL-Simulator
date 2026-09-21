# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  wavedb
  SOURCES FstWriter.cpp VcdWriter.cpp WaveformDatabase.cpp WaveformRecorder.cpp
          WaveformWriter.cpp
  HEADERS FstWriter.h VcdWriter.h WaveformDatabase.h WaveformRecorder.h WaveformWriter.h
  DEPENDS enzan::common enzan::eir enzan::runtime
  PRIVATE_DEPENDS enzan::fst)
