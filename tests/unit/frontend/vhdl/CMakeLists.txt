# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(frontend_vhdl SOURCES VhdlDesignTests.cpp VhdlFrontendTests.cpp
                    DEPENDS enzan::frontend_vhdl enzan::elab enzan::codegen enzan::runtime)

# Every analysis is against the standard libraries the build analysed.
add_dependencies(enzan_unittest_frontend_vhdl enzan-vhdl-libraries)
target_compile_definitions(enzan_unittest_frontend_vhdl
                           PRIVATE ENZAN_VHDL_LIBRARY_ROOT="${ENZAN_VHDL_LIBRARY_ROOT}")
