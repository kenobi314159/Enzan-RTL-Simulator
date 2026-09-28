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

std::string versionNotice(std::string_view programName) {
    std::string result = versionLine();
    result += '\n';
    result += buildDescription();

    // The notice is what the GPL asks an interactive program to display, and users of simulators
    // expect `--version` to say which one they are running.
    result += "\nCopyright (C) 2026 the Enzan authors.\n";
    result += "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n";
    result += "This is free software: you are free to change and redistribute it.\n";
    result += "There is NO WARRANTY, to the extent permitted by law.\n";

    // Named last rather than first: the version line above already says `enzan`, and prefixing
    // it with the program name produced `enzan-gui enzan 0.1.0-dev` in the front end.
    result += "\nInvoked as: ";
    result += programName;
    result += '\n';
    return result;
}

} // namespace enzan
