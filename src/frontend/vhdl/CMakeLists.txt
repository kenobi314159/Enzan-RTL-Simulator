# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  frontend_vhdl
  SOURCES Folder.cpp NvcAnalysis.cpp ProcessTranslator.cpp Translator.cpp Types.cpp VhdlFrontend.cpp
  HEADERS Folder.h NvcAnalysis.h NvcApi.h ProcessTranslator.h Translator.h Types.h VhdlFrontend.h
  DEPENDS enzan::common enzan::eir
  PRIVATE_DEPENDS enzan::nvc)
