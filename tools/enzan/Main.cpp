// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point for the headless simulation kernel.
//
// The kernel and the GUI are separate processes (DESIGN_GUIDE.md section 2.2).
// This executable must stay usable with no display attached, so nothing here
// may reach into src/gui or link Qt.

#include "common/Version.h"

#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

/// Exit statuses. `Usage` is distinct from a simulation failure so that scripts
/// can tell "you invoked me wrongly" from "the design failed".
enum class ExitStatus : std::uint8_t { Success = 0, Usage = 1 };

constexpr std::string_view UsageText =
    R"(Usage: enzan [options]

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and
VHDL. This build is at milestone M0: the build, diagnostic and test foundation
exists, but no analysis, elaboration or simulation capability has been
implemented yet, so there is nothing to run.

Options:
  -h, --help     Print this message and exit.
  -V, --version  Print version and build information and exit.

The normative specification, including the milestone roadmap, is DESIGN_GUIDE.md
in the source tree.
)";

void printVersion() {
    std::cout << enzan::versionLine() << '\n' << enzan::buildDescription();

    // The notice is what the GPL asks an interactive program to display, and
    // users of simulators expect `--version` to say which one they are running.
    std::cout << "\nCopyright (C) 2026 the Enzan authors.\n"
                 "License GPLv3+: GNU GPL version 3 or later "
                 "<https://gnu.org/licenses/gpl.html>.\n"
                 "This is free software: you are free to change and redistribute it.\n"
                 "There is NO WARRANTY, to the extent permitted by law.\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> rawArguments(argv, static_cast<std::size_t>(argc));
    const auto arguments = rawArguments.subspan(rawArguments.empty() ? 0 : 1);

    // No arguments is an error rather than a no-op: at this milestone there is
    // no default action, and silently succeeding would make a script that
    // forgot its arguments look as though it had run a simulation.
    if (arguments.empty()) {
        std::cerr << UsageText;
        return static_cast<int>(ExitStatus::Usage);
    }

    for (const char* rawArgument : arguments) {
        const std::string_view argument(rawArgument);

        if (argument == "-h" || argument == "--help") {
            std::cout << UsageText;
            return static_cast<int>(ExitStatus::Success);
        }

        if (argument == "-V" || argument == "--version") {
            printVersion();
            return static_cast<int>(ExitStatus::Success);
        }

        std::cerr << "enzan: unrecognised option '" << argument << "'\n"
                  << "Try 'enzan --help' for the available options.\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    return static_cast<int>(ExitStatus::Success);
}
