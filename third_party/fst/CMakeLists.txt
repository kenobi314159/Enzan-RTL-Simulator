# SPDX-License-Identifier: GPL-3.0-or-later
#
# GTKWave's FST library, vendored unmodified. See README.md in this directory.

add_library(enzan_fst STATIC fastlz.c fstapi.c lz4.c)
add_library(enzan::fst ALIAS enzan_fst)

# Deliberately NOT linked against enzan::compile_options.
#
# Enzan compiles with a wide warning set that CI turns into -Werror. Third-party C code held to
# it would fail the build over style Enzan does not own, and patching the sources to satisfy us
# would break the rule that keeps them byte-identical to upstream and the next update a straight
# replacement. So this target gets only what it needs to build.
target_compile_features(enzan_fst PRIVATE c_std_11)

# fstapi.c includes whatever FST_CONFIG_INCLUDE names, defaulting to GTKWave's autoconf
# <config.h>. Enzan has no autoconf, so it is pointed at fst_config.h instead — which is what the
# hook exists for, and why the vendored sources need no patching.
#
# The quoted form rather than angle brackets: `<` and `>` in a compile definition have to survive
# CMake, the generator and possibly a shell, and the quoted form searches beside the including
# file so it works whatever happens to the include path.
target_compile_definitions(enzan_fst PRIVATE FST_CONFIG_INCLUDE=\"fst_config.h\")

target_include_directories(enzan_fst PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# SYSTEM, so that a component including fstapi.h is not held to Enzan's warning set for the
# contents of a header it does not own.
target_include_directories(enzan_fst SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# FST blocks are zlib-compressed; lz4 and fastlz are vendored alongside.
target_link_libraries(enzan_fst PRIVATE ZLIB::ZLIB)

# Upstream's own warnings are not Enzan's business, and a sanitiser run over a library that is
# not being developed here is noise. The sanitiser flags are still applied when the rest of the
# build has them, because a genuine memory error in the writer would corrupt Enzan's output and
# is worth catching wherever it lives.
target_compile_options(enzan_fst PRIVATE -w)

if(ENZAN_SANITIZERS)
  list(JOIN ENZAN_SANITIZERS "," _enzan_fst_sanitizers)
  target_compile_options(enzan_fst PRIVATE -fsanitize=${_enzan_fst_sanitizers}
                                           -fno-omit-frame-pointer)
  target_link_options(enzan_fst PRIVATE -fsanitize=${_enzan_fst_sanitizers})
endif()
