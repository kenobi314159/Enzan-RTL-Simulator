# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  common
  SOURCES AssertTests.cpp DiagnosticsTests.cpp InstallationTests.cpp SourceManagerTests.cpp
          VersionTests.cpp
  DEPENDS enzan::common)

# Where the install rules put each part, and where this build put its own (section 11.5).
target_compile_definitions(
  enzan_unittest_common
  PRIVATE ENZAN_TEST_BUILD_BINDIR="${ENZAN_BUILD_BINDIR}"
          ENZAN_TEST_INSTALL_BINDIR="${ENZAN_INSTALL_BINDIR}"
          ENZAN_TEST_INSTALL_LIBEXECDIR="${ENZAN_INSTALL_LIBEXECDIR}"
          ENZAN_TEST_INSTALL_VHDLDIR="${ENZAN_INSTALL_VHDLDIR}")
if(TARGET enzan-gui)
  target_compile_definitions(enzan_unittest_common
                             PRIVATE ENZAN_TEST_FRONT_END="$<TARGET_FILE:enzan-gui>")
endif()
if(ENZAN_ENABLE_VHDL)
  target_compile_definitions(enzan_unittest_common
                             PRIVATE ENZAN_TEST_VHDL_LIBRARIES="${ENZAN_VHDL_LIBRARY_ROOT}")
endif()
