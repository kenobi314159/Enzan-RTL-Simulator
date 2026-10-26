# SPDX-License-Identifier: GPL-3.0-or-later
#
# The GUI tests supply their own `main`, because a QApplication has to exist before any widget
# and has to be told to use the offscreen platform before it is created. That is also why they
# do not link GTest::gtest_main.

set(CMAKE_AUTOMOC ON)

add_executable(
  enzan_unittest_gui
  DesignTreeModelTests.cpp
  GuiTestMain.cpp
  KernelSessionTests.cpp
  MainWindowTests.cpp
  ShortcutSheetTests.cpp
  SignalListLoadTests.cpp
  SignalListTests.cpp
  ThemeTests.cpp
  WaveRowTests.cpp
  WaveformInteractionTests.cpp
  TimeAxisTests.cpp
  WaveformViewTests.cpp)

target_link_libraries(
  enzan_unittest_gui
  PRIVATE enzan::compile_options enzan::test_support enzan::gui enzan::ipc enzan::ipc_server
          GTest::gtest Qt6::Widgets Qt6::Test)

# The fixture header sits beside the tests rather than in tests/unit/support, because it is
# specific to the GUI: it builds a kernel, serves it over a socket and attaches a session, which
# nothing outside this directory wants.
target_include_directories(enzan_unittest_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

gtest_discover_tests(
  enzan_unittest_gui
  TEST_PREFIX "unit.gui."
  PROPERTIES TIMEOUT 180
  DISCOVERY_MODE PRE_TEST)

# Qt's unprefixed `signals`, `slots` and `emit` keywords collide with the vocabulary the rest of
# Enzan is written in — `Image::signals()`, `ElaboratedDesign::slots()` and
# `DiagnosticEngine::emit` are each the right name for what they are — so the macros are turned
# off here and Q_SIGNALS, Q_SLOTS and Q_EMIT are used instead.
target_compile_definitions(enzan_unittest_gui PRIVATE QT_NO_KEYWORDS)
