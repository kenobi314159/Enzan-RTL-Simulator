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
  SOURCES Configuration.cpp
          DesignTreeModel.cpp
          KernelSession.cpp
          MainWindow.cpp
          ObjectsModel.cpp
          ShortcutSheet.cpp
          SignalList.cpp
          TclLibrary.cpp
          TclSyntax.cpp
          Theme.cpp
          TimeAxis.cpp
          TranscriptView.cpp
          WaveformModel.cpp
          WaveformView.cpp
  HEADERS Configuration.h
          DesignTreeModel.h
          KernelSession.h
          MainWindow.h
          ObjectsModel.h
          ShortcutSheet.h
          SignalList.h
          TclLibrary.h
          TclSyntax.h
          Theme.h
          ThemeColors.def
          TimeAxis.h
          TranscriptView.h
          WaveformModel.h
          WaveformView.h
  DEPENDS enzan::common enzan::eir enzan::ipc enzan::wavedb Qt6::Widgets Qt6::Gui Qt6::Core)

# The themes are compiled in rather than installed beside the binary. Section 10.5 makes the theme
# files the only place colours live, so they have to be there whatever the installation looks
# like; the window writes copies of them into the configuration directory for a user to change.
# A theme listed here is a built-in theme, named for its file: nothing else names them.
qt_add_resources(enzan_gui "enzan_gui_themes" PREFIX "/enzan" FILES themes/dark.tcl
                 themes/light.tcl)

target_include_directories(enzan_gui SYSTEM PRIVATE ${TCL_INCLUDE_PATH})
target_link_libraries(enzan_gui PRIVATE ${TCL_LIBRARY})

# Qt's unprefixed `signals`, `slots` and `emit` keywords collide with the vocabulary the rest of
# Enzan is written in — `Image::signals()`, `ElaboratedDesign::slots()` and
# `DiagnosticEngine::emit` are each the right name for what they are — so the macros are turned
# off here and Q_SIGNALS, Q_SLOTS and Q_EMIT are used instead.
target_compile_definitions(enzan_gui PRIVATE QT_NO_KEYWORDS)
