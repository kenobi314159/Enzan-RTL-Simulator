# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  pli
  SOURCES PliContextTests.cpp VpiLoadingTests.cpp VpiValueTests.cpp
  DEPENDS enzan::pli enzan::codegen enzan::elab)

enzan_use_test_plugins(enzan_unittest_pli)
