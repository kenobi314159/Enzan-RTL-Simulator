// SPDX-License-Identifier: GPL-3.0-or-later
//
// enzan-vhdl-library: analyses files into one of VHDL's standard libraries, as the build does
// for each of their files in turn (DESIGN_GUIDE.md section 4.4). A build-time tool, not something
// a user runs; CMakeLists.txt beside it says in which order and why.
//
//   enzan-vhdl-library --standard 1993|2008|2019 --library NAME:DIRECTORY
//                      [--search DIRECTORY]... [--bootstrap] FILE...
//
// Exit status 0 when every file analysed, 1 for a wrong command line, 2 when analysis failed, as
// the statuses of section 8.1.

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "frontend/vhdl/VhdlFrontend.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using enzan::frontend::vhdl::Standard;
using enzan::frontend::vhdl::UnitOptions;

std::optional<Standard> parseStandard(std::string_view text) {
    if (text == "1993") return Standard::Vhdl1993;
    if (text == "2008") return Standard::Vhdl2008;
    if (text == "2019") return Standard::Vhdl2019;
    return std::nullopt;
}

int usage(std::string_view problem) {
    std::cerr << "enzan-vhdl-library: " << problem << "\n"
              << "usage: enzan-vhdl-library --standard 1993|2008|2019 --library NAME:DIRECTORY\n"
              << "                          [--search DIRECTORY]... [--bootstrap] FILE...\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));

    UnitOptions options;
    std::vector<std::filesystem::path> files;
    bool haveStandard = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (index + 1 >= arguments.size()) return std::nullopt;
            return std::string_view(arguments[++index]);
        };

        if (argument == "--standard") {
            const std::optional<std::string_view> text = value();
            const std::optional<Standard> standard = text ? parseStandard(*text) : std::nullopt;
            if (!standard) return usage("--standard takes 1993, 2008 or 2019");
            options.standard = *standard;
            haveStandard = true;
        } else if (argument == "--library") {
            const std::optional<std::string_view> spec = value();
            if (!spec) return usage("--library takes NAME:DIRECTORY");
            const std::size_t colon = spec->find(':');
            if (colon == std::string_view::npos || colon == 0)
                return usage("--library takes NAME:DIRECTORY");
            options.work = {.name = std::string(spec->substr(0, colon)),
                            .directory = std::filesystem::path(spec->substr(colon + 1))};
        } else if (argument == "--search") {
            const std::optional<std::string_view> directory = value();
            if (!directory) return usage("--search takes a directory");
            options.searchDirectories.emplace_back(*directory);
        } else if (argument == "--bootstrap") {
            options.bootstrap = true;
        } else if (!argument.starts_with("-")) {
            files.emplace_back(argument);
        } else {
            return usage("unexpected argument '" + std::string(argument) + "'");
        }
    }
    if (!haveStandard || options.work.name.empty() || files.empty())
        return usage("--standard, --library and a file are all required");

    enzan::SourceManager sources;
    enzan::DiagnosticEngine diagnostics(sources);
    enzan::TextDiagnosticConsumer consumer(sources, std::cerr);
    diagnostics.addConsumer(consumer);

    for (const std::filesystem::path& file : files) {
        const enzan::FileReadResult read = sources.readFile(file);
        if (!read.ok()) {
            std::cerr << "enzan-vhdl-library: cannot read " << file.string() << "\n";
            return 2;
        }
        options.sources.push_back(read.buffer);
    }

    // NVC creates a library's own directory and not the ones above it.
    std::error_code failure;
    std::filesystem::create_directories(options.work.directory.parent_path(), failure);

    return enzan::frontend::vhdl::analyseUnits(options, sources, diagnostics) ? 0 : 2;
}
