# SPDX-License-Identifier: GPL-3.0-or-later
#
# The part of NVC the VHDL frontend uses: its analysis, built from the submodule in
# ../nvc without NVC's autotools. See README.md here and docs/development/nvc_integration.md.

set(_nvc "${CMAKE_CURRENT_SOURCE_DIR}/../nvc")

if(NOT EXISTS "${_nvc}/src/parse.c")
  message(
    FATAL_ERROR
      "ENZAN_ENABLE_VHDL is ON, but NVC's sources are missing from third_party/nvc.\n"
      "They are a git submodule; fetch them with:\n"
      "  git submodule update --init third_party/nvc\n"
      "or configure with -DENZAN_ENABLE_VHDL=OFF to build without the VHDL frontend.")
endif()

# The version comes from NVC's own AC_INIT, so that the submodule's commit is the one place the
# pinned revision is recorded.
file(STRINGS "${_nvc}/configure.ac" _nvc_init REGEX "^AC_INIT\\(")
if(NOT _nvc_init MATCHES "^AC_INIT\\(\\[nvc\\], *\\[([^]]+)\\]")
  message(FATAL_ERROR "Cannot read NVC's version from third_party/nvc/configure.ac.")
endif()
set(ENZAN_NVC_VERSION "${CMAKE_MATCH_1}")
message(STATUS "Using NVC ${ENZAN_NVC_VERSION} (submodule) for VHDL analysis")

# What NVC's configure would have probed, for the config.h that stands in for its own.
include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckStructHasMember)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(getline "stdio.h" HAVE_GETLINE)
check_symbol_exists(strndup "string.h" HAVE_STRNDUP)
check_symbol_exists(strchrnul "string.h" HAVE_STRCHRNUL)
check_symbol_exists(strcasestr "string.h" HAVE_STRCASESTR)
check_symbol_exists(memmem "string.h" HAVE_MEMMEM)
check_symbol_exists(fseeko "stdio.h" HAVE_FSEEKO)
check_symbol_exists(ftello "stdio.h" HAVE_FTELLO)
check_symbol_exists(gettid "unistd.h" HAVE_GETTID)
check_symbol_exists(getcontext "ucontext.h" HAVE_GETCONTEXT)
check_symbol_exists(tcgetwinsize "termios.h" HAVE_TCGETWINSIZE)
check_include_file(ucontext.h HAVE_UCONTEXT_H)
check_include_file(sys/ucontext.h HAVE_SYS_UCONTEXT_H)
check_include_file(sys/prctl.h HAVE_SYS_PRCTL_H)
check_include_file(sys/ptrace.h HAVE_SYS_PTRACE_H)
check_struct_has_member("struct stat" st_mtim.tv_nsec "sys/stat.h"
                        HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC LANGUAGE C)
unset(CMAKE_REQUIRED_DEFINITIONS)

set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(nvc_config.h.in "${_generated}/config.h" @ONLY)

flex_target(enzan_nvc_lexer "${_nvc}/src/lexer.l" "${_generated}/lexer.c")

# The files analysis needs once NVC's constant evaluator is replaced, as measured by the spike;
# docs/development/nvc_integration.md says how the list was derived and how to derive it again.
set(_nvc_sources
    src/bounds.c
    src/common.c
    src/debug.c
    src/diag.c
    src/fbuf.c
    src/hash.c
    src/ident.c
    src/lib.c
    src/mask.c
    src/names.c
    src/object.c
    src/option.c
    src/parse.c
    src/printf.c
    src/scan.c
    src/sem.c
    src/simp.c
    src/thread.c
    src/tree.c
    src/type.c
    src/util.c
    src/psl/psl-node.c
    src/psl/psl-sem.c
    src/rt/mspace.c
    src/sdf/sdf-parse.c
    src/sdf/sdf-util.c
    src/vhdl/vhdl-inst.c
    src/vhdl/vhdl-util.c
    src/vlog/vlog-node.c
    src/vlog/vlog-number.c
    src/vlog/vlog-parse.c
    src/vlog/vlog-symtab.c
    src/vlog/vlog-util.c
    thirdparty/cpustate.c
    thirdparty/fastlz.c
    thirdparty/sha1.c)

# NVC compiles a replacement for each C library function its configure found missing.
foreach(_function getline strndup strchrnul strcasestr memmem)
  string(TOUPPER "HAVE_${_function}" _have)
  if(NOT ${_have})
    list(APPEND _nvc_sources "thirdparty/${_function}.c")
  endif()
endforeach()

list(TRANSFORM _nvc_sources PREPEND "${_nvc}/")

add_library(enzan_nvc STATIC ${_nvc_sources} ${FLEX_enzan_nvc_lexer_OUTPUTS} enzan_nvc_seam.c)
add_library(enzan::nvc ALIAS enzan_nvc)

# NVC is C11 with GNU extensions: cleanup attributes and statement expressions throughout.
set_target_properties(enzan_nvc PROPERTIES C_STANDARD 11 C_EXTENSIONS ON
                                           POSITION_INDEPENDENT_CODE ON)

# NVC's headers as C++ can include them.
#
# The adapter that walks NVC's trees is C++, and NVC's headers are C that C++ rejects in exactly
# three ways: two parameters named `new` and one named `class`, and C11's `_Static_assert`. Each is
# rewritten here, from the pinned sources at configure time, into a copy of the header tree the
# adapter includes instead. Redefining the keywords around an #include would do the same with
# less, but a translation unit that also includes a standard header may not define a keyword's
# name (C++ [macro.names]), and a hand-edited copy would drift from the pin.
set(_cxx "${_generated}/cxx")
file(GLOB_RECURSE _nvc_headers RELATIVE "${_nvc}" CONFIGURE_DEPENDS "${_nvc}/src/*.h"
     "${_nvc}/thirdparty/*.h")
foreach(_header IN LISTS _nvc_headers)
  file(READ "${_nvc}/${_header}" _text)
  string(REGEX REPLACE "([A-Za-z0-9_*][ \t]+[*]*)(new|class)([ \t]*[,)])" "\\1\\2_\\3" _text
                       "${_text}")
  string(REPLACE "_Static_assert(" "static_assert(" _text "${_text}")
  # Written only when it changed, so that reconfiguring rebuilds nothing that includes it.
  set(_existing "")
  if(EXISTS "${_cxx}/${_header}")
    file(READ "${_cxx}/${_header}" _existing)
  endif()
  if(NOT _existing STREQUAL _text)
    file(WRITE "${_cxx}/${_header}" "${_text}")
  endif()
endforeach()

# Every NVC header includes config.h only when HAVE_CONFIG_H is defined, so a component
# including NVC's headers needs the definition and the directory as much as NVC does.
target_compile_definitions(enzan_nvc PUBLIC HAVE_CONFIG_H)
# NVC's own root as well: some of its files include `thirdparty/...` from there, as its automake
# build allows by putting the source root on every include path.
target_include_directories(enzan_nvc PRIVATE "${_nvc}/src" "${_nvc}/thirdparty" "${_nvc}"
                                             "${_generated}")

# The C++ copies, not the originals. SYSTEM, so that the adapter is not held to Enzan's warning
# set for code it does not own.
target_include_directories(enzan_nvc SYSTEM INTERFACE "${_cxx}/src" "${_cxx}/thirdparty"
                                                      "${_cxx}" "${_generated}")

# GTKWave's FST writer, which Enzan also builds, vendors the same FastLZ under the same names. Two
# static libraries defining one symbol link only because the linker takes whichever it meets
# first, so NVC's copy is renamed where it is compiled; the sources stay untouched.
target_compile_definitions(
  enzan_nvc PRIVATE fastlz_compress=enzan_nvc_fastlz_compress
                    fastlz_compress_level=enzan_nvc_fastlz_compress_level
                    fastlz_decompress=enzan_nvc_fastlz_decompress)

# NVC's own warnings are not Enzan's business, as with the FST library beside it.
target_compile_options(enzan_nvc PRIVATE -w)

find_package(Threads REQUIRED)
target_link_libraries(enzan_nvc PUBLIC ZSTD::ZSTD Threads::Threads m)

# The sanitisers apply here as they do to Enzan, since the adapter hands NVC's trees around and a
# memory error on either side of that boundary is worth finding. NVC detects AddressSanitizer
# itself and tells its collector which memory it may scan (src/util.h).
if(ENZAN_SANITIZERS)
  list(JOIN ENZAN_SANITIZERS "," _enzan_nvc_sanitizers)
  target_compile_options(enzan_nvc PRIVATE -fsanitize=${_enzan_nvc_sanitizers}
                                           -fno-omit-frame-pointer)
  target_link_options(enzan_nvc PRIVATE -fsanitize=${_enzan_nvc_sanitizers})

  # Except Clang's check that a function is called through a pointer of its own type. NVC's trees
  # are a typed view of one object store, and it hands a callback written for trees to the store's
  # rewriter through a cast (src/object.c), on every rewrite of every unit. That is its design,
  # sound on every ABI Enzan targets, and nothing to act on without patching NVC; the rest of
  # UndefinedBehaviorSanitizer stays on.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(enzan_nvc PRIVATE -fno-sanitize=function)
  endif()
endif()
