# SPDX-License-Identifier: GPL-3.0-or-later
#
# The graphical front end (DESIGN_GUIDE.md section 10).
#
# It links the GUI component and nothing from the simulator: the kernel is a separate process
# (section 2.2), and a dependency on `runtime` or on a frontend here would undo the separation
# that lets a local window drive a kernel on another machine.

set(CMAKE_AUTOMOC ON)

add_executable(enzan-gui Main.cpp)
target_link_libraries(enzan-gui PRIVATE enzan::compile_options enzan::common enzan::gui
                                        Qt6::Widgets)

if(ENZAN_BUILD_TESTS)
  # End-to-end checks on the executable, which run with no display because they exit before a
  # window is shown. The offscreen platform keeps them runnable on a CI machine with no X or
  # Wayland session at all.
  add_test(NAME cli.enzan-gui.version COMMAND enzan-gui --version)
  set_tests_properties(cli.enzan-gui.version PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                       PASS_REGULAR_EXPRESSION "[0-9]+\\.[0-9]+\\.[0-9]+")

  add_test(NAME cli.enzan-gui.help COMMAND enzan-gui --help)
  set_tests_properties(cli.enzan-gui.help PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                       PASS_REGULAR_EXPRESSION "--attach")

  add_test(NAME cli.enzan-gui.unknown_option COMMAND enzan-gui --no-such-option)
  set_tests_properties(cli.enzan-gui.unknown_option PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen" WILL_FAIL TRUE)

  # The built-in theme has to be writable out and has to parse when read back, which is what
  # makes `--write-theme` a usable starting point rather than a decoration.
  add_test(NAME cli.enzan-gui.write_theme
           COMMAND enzan-gui --write-theme "${CMAKE_CURRENT_BINARY_DIR}/written-theme.tcl")
  set_tests_properties(cli.enzan-gui.write_theme PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                       PASS_REGULAR_EXPRESSION "wrote the built-in theme")
endif()

# Qt's unprefixed `signals`, `slots` and `emit` keywords collide with the vocabulary the rest of
# Enzan is written in — `Image::signals()`, `ElaboratedDesign::slots()` and
# `DiagnosticEngine::emit` are each the right name for what they are — so the macros are turned
# off here and Q_SIGNALS, Q_SLOTS and Q_EMIT are used instead.
target_compile_definitions(enzan-gui PRIVATE QT_NO_KEYWORDS)
