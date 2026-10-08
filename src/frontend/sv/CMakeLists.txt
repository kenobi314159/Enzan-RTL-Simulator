# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  frontend_sv
  SOURCES DeadCode.cpp ParameterLowering.cpp ProcessTranslator.cpp SvIntConversion.cpp
          SymbolicWidths.cpp SystemVerilogFrontend.cpp
  HEADERS DeadCode.h ParameterLowering.h ProcessTranslator.h SvIntConversion.h
          SymbolicWidths.h SystemVerilogFrontend.h
  DEPENDS enzan::common enzan::eir
  PRIVATE_DEPENDS slang::slang)
