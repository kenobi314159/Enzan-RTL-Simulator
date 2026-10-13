# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  frontend_vhdl
  SOURCES BodyTranslator.cpp Folder.cpp NvcAnalysis.cpp Translator.cpp Types.cpp VhdlFrontend.cpp
  HEADERS BodyTranslator.h Folder.h NvcAnalysis.h NvcApi.h Translator.h Types.h VhdlFrontend.h
  DEPENDS enzan::common enzan::eir
  PRIVATE_DEPENDS enzan::nvc)
