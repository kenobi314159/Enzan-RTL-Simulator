# SPDX-License-Identifier: GPL-3.0-or-later

enzan_add_component(
  runtime
  SOURCES Format.cpp Image.cpp Interpreter.cpp Kernel.cpp ReadMem.cpp Scheduler.cpp Time.cpp
  HEADERS Arena.h
          Format.h
          Image.h
          Interpreter.h
          Kernel.h
          ReadMem.h
          Scheduler.h
          Time.h
  DEPENDS enzan::common enzan::eir enzan::elab enzan::codegen)
