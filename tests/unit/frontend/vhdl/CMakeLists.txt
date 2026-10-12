# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(frontend_vhdl SOURCES VhdlFrontendTests.cpp
                    DEPENDS enzan::frontend_vhdl)

# The suite bootstraps `std` from the source NVC ships, since every analysis needs it.
target_compile_definitions(
  enzan_unittest_frontend_vhdl
  PRIVATE ENZAN_NVC_SOURCE_DIR="${PROJECT_SOURCE_DIR}/third_party/nvc")
