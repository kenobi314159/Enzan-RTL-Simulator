# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_unit_test(
  elab
  SOURCES BoundaryTests.cpp ElaboratorTests.cpp
  DEPENDS enzan::elab enzan::codegen enzan::runtime)
