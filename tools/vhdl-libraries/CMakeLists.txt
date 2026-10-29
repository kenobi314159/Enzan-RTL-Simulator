# SPDX-License-Identifier: GPL-3.0-or-later
#
# VHDL's standard libraries, `std`, `ieee` and NVC's own `nvc`, analysed when Enzan is built and
# found beside the executable at run time.
#
# They live in lib/enzan/vhdl, beside bin/, in the build tree as in an installation, one directory
# per library and standard as NVC lays them out: `std` and `ieee` for 1993, which serve 2000 and
# 2002 as well, then `std.08`, `std.19` and so on. A library is analysed from the sources NVC ships,
# by enzan-vhdl-library, one source file per run as NVC's own build does (docs/nvc_integration.md).

add_executable(enzan-vhdl-library Main.cpp)
target_link_libraries(enzan-vhdl-library PRIVATE enzan::compile_options enzan::common
                                                 enzan::frontend_vhdl)

set(_nvc "${PROJECT_SOURCE_DIR}/third_party/nvc")
set(ENZAN_VHDL_LIBRARY_ROOT
    "${ENZAN_BUILD_VHDLDIR}"
    CACHE INTERNAL "Where the build puts VHDL's standard libraries")

# The order is NVC's, read from the deps.mk files its tree carries beside each library's sources:
# one line per unit, naming the unit, the file it is analysed from and every unit it needs first.
# Reading them rather than restating them means an NVC upgrade that adds a unit or a dependency
# changes Enzan's build with it.
#
# One build step per source file rather than per unit, since a package and its body often share a
# file, and analysing that file again for the body would replace the package it had just stored.
# Steps that do not depend on each other run in parallel: NVC holds a lock on a library while it
# writes one, so two analyses into the same library at once is what NVC's own parallel build does.
#
# Each step's output is a stamp beside this file's build directory rather than the units it
# writes. A build system creates the directory of every output before running the step, and NVC
# rightly warns about turning a directory it did not create into a library; with the stamps
# elsewhere, only NVC creates the libraries.
set(_stamps "${CMAKE_CURRENT_BINARY_DIR}/stamps")
set(_all)
foreach(_standard 1993 2008 2019)
  if(_standard STREQUAL "1993")
    set(_suffix "")
  else()
    string(SUBSTRING "${_standard}" 2 2 _two)
    set(_suffix ".${_two}")
  endif()

  # First every unit, so that a step can depend on the step writing a unit it needs.
  set(_sources)
  foreach(_library std nvc ieee)
    file(STRINGS "${_nvc}/lib/${_library}${_suffix}/deps.mk" _lines REGEX "^lib/")
    foreach(_line IN LISTS _lines)
      string(REPLACE "$(top_srcdir)/" "" _line "${_line}")
      if(NOT _line MATCHES "^lib/([^/]+)/([^:]+): +([^ ]+)(.*)$")
        message(FATAL_ERROR "Cannot read NVC's library dependencies: ${_line}")
      endif()
      set(_directory "${CMAKE_MATCH_1}")
      set(_unit "${CMAKE_MATCH_2}")
      set(_source "${CMAKE_MATCH_3}")
      string(STRIP "${CMAKE_MATCH_4}" _needs)

      # VITAL is the gate-level timing library, and gate-level simulation is out of scope.
      if(_source MATCHES "^lib/vital/")
        continue()
      endif()

      string(MAKE_C_IDENTIFIER "${_standard}_${_source}" _key)
      if(NOT _source IN_LIST _sources)
        list(APPEND _sources "${_source}")
        set(_needs_${_key})
        set(_bootstrap_${_key})
        set(_directory_${_key} "${_directory}")
      endif()

      string(MAKE_C_IDENTIFIER "${_directory}/${_unit}" _unit_key)
      set(_stamp_of_${_unit_key} "${_stamps}/${_key}")

      # std.standard is what every other unit is analysed against, so it alone is analysed in
      # NVC's bootstrap mode.
      if(_unit STREQUAL "STD.STANDARD")
        set(_bootstrap_${_key} --bootstrap)
      endif()

      separate_arguments(_needs)
      list(APPEND _needs_${_key} ${_needs})
    endforeach()
  endforeach()

  foreach(_source IN LISTS _sources)
    string(MAKE_C_IDENTIFIER "${_standard}_${_source}" _key)
    set(_directory "${_directory_${_key}}")
    string(REGEX REPLACE "\\..*$" "" _name "${_directory}")

    # A unit needs nothing of the file it comes from, which this step writes itself.
    set(_depends "${_nvc}/${_source}")
    foreach(_need IN LISTS _needs_${_key})
      string(REGEX REPLACE "^lib/" "" _need "${_need}")
      string(MAKE_C_IDENTIFIER "${_need}" _unit_key)
      if(NOT DEFINED _stamp_of_${_unit_key})
        message(FATAL_ERROR "NVC's ${_source} needs ${_need}, which no source provides")
      endif()
      if(NOT _stamp_of_${_unit_key} STREQUAL "${_stamps}/${_key}")
        list(APPEND _depends "${_stamp_of_${_unit_key}}")
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _depends)

    add_custom_command(
      OUTPUT "${_stamps}/${_key}"
      COMMAND
        enzan-vhdl-library --standard ${_standard} --library
        "${_name}:${ENZAN_VHDL_LIBRARY_ROOT}/${_directory}" --search "${ENZAN_VHDL_LIBRARY_ROOT}"
        ${_bootstrap_${_key}} "${_nvc}/${_source}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${_stamps}/${_key}"
      DEPENDS enzan-vhdl-library ${_depends}
      COMMENT "Analysing VHDL-${_standard} ${_name}: ${_source}"
      VERBATIM)
    list(APPEND _all "${_stamps}/${_key}")
  endforeach()
endforeach()

add_custom_target(enzan-vhdl-libraries ALL DEPENDS ${_all})

# Every library the steps above wrote, as they wrote it. An installation's copy is one nothing
# writes to, and usually one its user cannot.
install(
  DIRECTORY "${ENZAN_VHDL_LIBRARY_ROOT}/"
  DESTINATION "${ENZAN_INSTALL_VHDLDIR}"
  COMPONENT enzan)
