# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Verilog Procedural Interface. The headers applications compile against are in
# include/enzan/, because they belong to the applications rather than to Enzan.

add_library(enzan_vpi_headers INTERFACE)
add_library(enzan::vpi_headers ALIAS enzan_vpi_headers)
target_include_directories(enzan_vpi_headers INTERFACE "${PROJECT_SOURCE_DIR}/include/enzan")

# VHPI is the VHDL frontend's, and its header is the IEEE P1076 working group's, which the
# NVC that frontend is built on carries.
set(ENZAN_VHPI_SOURCES "")
if(ENZAN_ENABLE_VHDL)
  set(ENZAN_VHPI_SOURCES Vhpi.cpp VhpiRoutines.cpp)
endif()

enzan_add_component(
  pli
  SOURCES PliContext.cpp VpiRoutines.cpp VpiValue.cpp ${ENZAN_VHPI_SOURCES}
  HEADERS PliContext.h PliModel.h VpiValue.h
  DEPENDS enzan::common enzan::eir enzan::runtime enzan::vpi_headers
  PRIVATE_DEPENDS enzan::elab ${CMAKE_DL_LIBS})

if(ENZAN_ENABLE_VHDL)
  target_include_directories(enzan_pli SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/nvc/src/vhpi")
endif()

# The headers an application is compiled against to be loaded with `--vpi`, and with `--vhpi` the
# working group's, which Enzan's VHPI is compiled against too. An application loads into a
# simulator with a frontend, so a build without one installs none.
if(ENZAN_ENABLE_SYSTEMVERILOG OR ENZAN_ENABLE_VHDL)
  install(
    FILES "${PROJECT_SOURCE_DIR}/include/enzan/vpi_user.h"
          "${PROJECT_SOURCE_DIR}/include/enzan/sv_vpi_user.h"
    DESTINATION "${ENZAN_INSTALL_INCLUDEDIR}"
    COMPONENT enzan)
endif()
if(ENZAN_ENABLE_VHDL)
  install(
    FILES "${PROJECT_SOURCE_DIR}/third_party/nvc/src/vhpi/vhpi_user.h"
    DESTINATION "${ENZAN_INSTALL_INCLUDEDIR}"
    COMPONENT enzan)
endif()

# ---------------------------------------------------------------------------
# enzan_export_vpi(<target>)
# ---------------------------------------------------------------------------
#
# Makes an executable offer the `vpi_*` routines to the libraries it loads, and nothing else. An
# application resolves them against the process that loads it; exporting every symbol instead
# would put slang's and the standard library's where an application's own copies could collide
# with them.
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
