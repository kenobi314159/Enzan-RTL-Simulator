# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  frontend_sv
  SOURCES DeadCode.cpp ProcessTranslator.cpp SvIntConversion.cpp SystemVerilogFrontend.cpp
  HEADERS DeadCode.h ProcessTranslator.h SvIntConversion.h SystemVerilogFrontend.h
  DEPENDS enzan::common enzan::eir
  PRIVATE_DEPENDS slang::slang)
