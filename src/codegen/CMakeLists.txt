# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  codegen
  SOURCES Compiler.cpp
  HEADERS Bytecode.h Compiler.h
  DEPENDS enzan::common enzan::eir)
