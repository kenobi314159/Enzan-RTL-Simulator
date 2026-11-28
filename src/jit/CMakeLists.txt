# SPDX-License-Identifier: GPL-3.0-or-later

# Prefer the distribution's shared LLVM where it provides one. Otherwise link
# the ORC and host target components and their transitive dependencies.
if(TARGET LLVM AND LLVM_LINK_LLVM_DYLIB)
  set(_enzan_llvm LLVM)
else()
  llvm_map_components_to_libnames(_enzan_llvm Core OrcJIT native)
endif()

enzan_add_component(
  jit
  SOURCES Backend.cpp Engine.cpp
  HEADERS Backend.h Engine.h
  DEPENDS enzan::common
  PRIVATE_DEPENDS enzan::runtime ${_enzan_llvm})
target_include_directories(enzan_jit SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
