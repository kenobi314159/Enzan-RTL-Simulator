# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Qt 6 Widgets front end (DESIGN_GUIDE.md section 10).

# Qt's code generators are selected per target from these, so they must be set before the
# component's library is created. AUTOUIC is off: the layouts here are built in code, because a
# waveform view and a docked tool window are not what a form designer is good at.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

enzan_add_component(
  gui
  SOURCES DesignTreeModel.cpp
          KernelSession.cpp
          MainWindow.cpp
          ObjectsModel.cpp
          SourceView.cpp
          Theme.cpp
          TimeAxis.cpp
          TranscriptView.cpp
          WaveformModel.cpp
          WaveformView.cpp
  HEADERS DesignTreeModel.h
          KernelSession.h
          MainWindow.h
          ObjectsModel.h
          SourceView.h
          Theme.h
          ThemeColors.def
          TimeAxis.h
          TranscriptView.h
          WaveformModel.h
          WaveformView.h
  DEPENDS enzan::common enzan::eir enzan::ipc enzan::wavedb Qt6::Widgets Qt6::Gui Qt6::Core)

# The theme is compiled in rather than installed beside the binary. Section 10.5 makes the theme
# file the only place colours live, so it has to be there whatever the installation looks like;
# `enzan-gui --write-theme` hands a copy to a user who wants to change one.
qt_add_resources(enzan_gui "enzan_gui_themes" PREFIX "/enzan" FILES themes/dark.tcl)

target_include_directories(enzan_gui SYSTEM PRIVATE ${TCL_INCLUDE_PATH})
target_link_libraries(enzan_gui PRIVATE ${TCL_LIBRARY})

# Qt's unprefixed `signals`, `slots` and `emit` keywords collide with the vocabulary the rest of
# Enzan is written in — `Image::signals()`, `ElaboratedDesign::slots()` and
# `DiagnosticEngine::emit` are each the right name for what they are — so the macros are turned
# off here and Q_SIGNALS, Q_SLOTS and Q_EMIT are used instead.
target_compile_definitions(enzan_gui PRIVATE QT_NO_KEYWORDS)
