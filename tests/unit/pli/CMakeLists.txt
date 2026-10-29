# SPDX-License-Identifier: GPL-3.0-or-later

# VHPI is the VHDL frontend's, and is tested where it is built.
set(ENZAN_VHPI_TESTS "")
if(ENZAN_ENABLE_VHDL)
  set(ENZAN_VHPI_TESTS VhpiTests.cpp MixedLanguageTests.cpp)
endif()

enzan_add_unit_test(
  pli
  SOURCES PliContextTests.cpp VpiLoadingTests.cpp VpiValueTests.cpp ${ENZAN_VHPI_TESTS}
  DEPENDS enzan::pli enzan::codegen enzan::elab)

enzan_use_test_plugins(enzan_unittest_pli)
