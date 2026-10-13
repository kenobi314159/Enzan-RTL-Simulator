# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Verilog Procedural Interface (DESIGN_GUIDE.md section 9). The headers applications compile
# against are in include/enzan/, because they belong to the applications rather than to Enzan.

add_library(enzan_vpi_headers INTERFACE)
add_library(enzan::vpi_headers ALIAS enzan_vpi_headers)
target_include_directories(enzan_vpi_headers INTERFACE "${PROJECT_SOURCE_DIR}/include/enzan")

enzan_add_component(
  pli
  SOURCES PliContext.cpp VpiRoutines.cpp VpiValue.cpp
  HEADERS PliContext.h VpiValue.h
  DEPENDS enzan::common enzan::eir enzan::runtime enzan::vpi_headers
  PRIVATE_DEPENDS enzan::elab ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# enzan_export_vpi(<target>)
# ---------------------------------------------------------------------------
#
# Makes an executable offer the `vpi_*` routines to the libraries it loads, and nothing else
# (section 9.1). An application resolves them against the process that loads it; exporting every
# symbol instead would put slang's and the standard library's where an application's own copies
# could collide with them.
set(ENZAN_VPI_DYNAMIC_LIST
    "${CMAKE_CURRENT_SOURCE_DIR}/vpi.dynamic-list"
    CACHE INTERNAL "The linker list of symbols a VPI host exports")

function(enzan_export_vpi target)
  target_link_options(${target} PRIVATE "LINKER:--dynamic-list=${ENZAN_VPI_DYNAMIC_LIST}")
  set_property(
    TARGET ${target}
    APPEND
    PROPERTY LINK_DEPENDS "${ENZAN_VPI_DYNAMIC_LIST}")
endfunction()
