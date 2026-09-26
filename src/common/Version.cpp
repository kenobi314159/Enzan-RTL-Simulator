// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Version.h"

#include "common/VersionInfo.h"

namespace enzan {

std::string_view version() noexcept {
    return ENZAN_VERSION_STRING;
}

std::string_view sourceRevision() noexcept {
    return ENZAN_SOURCE_REVISION;
}

std::string versionLine() {
    return std::string("enzan ") + ENZAN_VERSION_STRING + " (" + ENZAN_SOURCE_REVISION + ")";
}

std::string buildDescription() {
    std::string result;
    result += "Compiler: ";
    result += ENZAN_BUILD_COMPILER;
    result += "\nBuild type: ";
    result += ENZAN_BUILD_TYPE;
    result += "\nOptional components:";

    // Reported unconditionally, including when nothing is enabled, so that the
    // absence of a component in a bug report is informative rather than
    // ambiguous.
    bool any = false;

    // Marked maybe_unused because a build with every optional component turned
    // off — which is a legitimate configuration — expands none of the uses below.
    [[maybe_unused]] const auto append = [&](const char* name) {
        result += any ? ", " : " ";
        result += name;
        any = true;
    };

#ifdef ENZAN_WITH_SYSTEMVERILOG
    append("systemverilog");
#endif
#ifdef ENZAN_WITH_VHDL
    append("vhdl");
#endif
#ifdef ENZAN_WITH_JIT
    append("jit");
#endif
#ifdef ENZAN_WITH_SANITIZERS
    append("sanitizers");
#endif

    if (!any) result += " none";
    result += '\n';
    return result;
}

} // namespace enzan
