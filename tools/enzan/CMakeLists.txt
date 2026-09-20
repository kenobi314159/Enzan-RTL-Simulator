# SPDX-License-Identifier: GPL-3.0-or-later
#
# The headless kernel. It must remain usable with no display attached
# (DESIGN_GUIDE.md section 2.2), so it must never gain a dependency on Qt or on
# anything under src/gui.

add_executable(enzan Main.cpp)
target_link_libraries(enzan PRIVATE enzan::compile_options enzan::common)

if(ENZAN_BUILD_TESTS)
  # End-to-end checks on the executable itself. The unit tests cover the version
  # strings; these cover the thing a user actually invokes, including the error
  # path for an option that does not exist.
  add_test(NAME cli.enzan.version COMMAND enzan --version)
  set_tests_properties(cli.enzan.version PROPERTIES PASS_REGULAR_EXPRESSION
                                                    "^enzan [0-9]+\\.[0-9]+\\.[0-9]+")

  add_test(NAME cli.enzan.help COMMAND enzan --help)
  set_tests_properties(cli.enzan.help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: enzan")

  # WILL_FAIL and PASS_REGULAR_EXPRESSION are mutually exclusive in CTest: when
  # a pass regex is set the exit status is ignored. The exit status and the
  # diagnostic text are therefore checked by two separate tests.
  add_test(NAME cli.enzan.unknown_option_status COMMAND enzan --no-such-option)
  set_tests_properties(cli.enzan.unknown_option_status PROPERTIES WILL_FAIL TRUE)

  add_test(NAME cli.enzan.unknown_option_message COMMAND enzan --no-such-option)
  set_tests_properties(cli.enzan.unknown_option_message
                       PROPERTIES PASS_REGULAR_EXPRESSION "unrecognised option")

  add_test(NAME cli.enzan.no_arguments COMMAND enzan)
  set_tests_properties(cli.enzan.no_arguments PROPERTIES WILL_FAIL TRUE)
endif()
