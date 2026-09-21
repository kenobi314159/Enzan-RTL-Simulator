// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point for the headless simulation kernel.
//
// The kernel and the GUI are separate processes (DESIGN_GUIDE.md section 2.2). This
// executable must stay usable with no display attached, so nothing here may reach into
// src/gui or link Qt.
//
// The command line is deliberately minimal. Section 8.1 commits to Tcl as *the* control
// language with a Questa-style command vocabulary, and that arrives with M3; adding a broad
// set of switches now would create a second control surface to keep compatible with it. What
// is here is enough to analyse, elaborate and run one design, which is what M1 needs.

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "common/Version.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef ENZAN_WITH_SYSTEMVERILOG
#include "codegen/Compiler.h"
#include "eir/Design.h"
#include "elab/Elaborator.h"
#include "frontend/sv/SystemVerilogFrontend.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"
#include "wavedb/VcdWriter.h"
#endif

namespace {

/// Exit statuses. `Usage` is distinct from a simulation failure so that scripts can tell
/// "you invoked me wrongly" from "the design failed".
enum class ExitStatus : std::uint8_t { Success = 0, Usage = 1, Failure = 2 };

constexpr std::string_view UsageText =
    R"(Usage: enzan [options] <file.sv>...

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and VHDL.

Options:
  -h, --help              Print this message and exit.
  -V, --version           Print version and build information and exit.
  -t, --top <module>      Module to elaborate as the top level. Required only when the
                          sources contain more than one candidate.
  -r, --run <time>        Stop after this many time units. The default runs until the
                          design finishes or runs out of events.
      --vcd <file>        Write a VCD waveform to this file.
      --vcd-scope <path>  Record only this subtree, for example /tb/u_dut.
      --timescale <unit>  Time unit used for delays and reported times: s, ms, us, ns,
                          ps or fs. The default is ps.
      --delta-limit <n>   Delta cycles allowed at one timestamp before the run is
                          abandoned. The default is 10000.
  -D, --define <NAME[=V]> Define a preprocessor macro.
  -I, --incdir <dir>      Add a directory to the `include search path.
      --dump-stats        Print instruction and delta counts when the run ends.

The normative specification, including the milestone roadmap, is DESIGN_GUIDE.md in the
source tree.
)";

void printVersion() {
    std::cout << enzan::versionLine() << '\n' << enzan::buildDescription();

    // The notice is what the GPL asks an interactive program to display, and users of
    // simulators expect `--version` to say which one they are running.
    std::cout << "\nCopyright (C) 2026 the Enzan authors.\n"
                 "License GPLv3+: GNU GPL version 3 or later "
                 "<https://gnu.org/licenses/gpl.html>.\n"
                 "This is free software: you are free to change and redistribute it.\n"
                 "There is NO WARRANTY, to the extent permitted by law.\n";
}

/// Everything the command line can ask for.
struct Invocation {
    std::vector<std::filesystem::path> files;
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> includeDirectories;
    std::string topModule;
    std::optional<std::uint64_t> runUntil;
    std::filesystem::path vcdPath;
    std::string vcdScope;
    std::int8_t timeExponent = -12;
    std::uint32_t deltaLimit = 10'000;
    bool dumpStatistics = false;
};

/// Parses a time unit name into its power-of-ten exponent.
std::optional<std::int8_t> parseTimeUnit(std::string_view text) {
    if (text == "s") return 0;
    if (text == "ms") return -3;
    if (text == "us") return -6;
    if (text == "ns") return -9;
    if (text == "ps") return -12;
    if (text == "fs") return -15;
    return std::nullopt;
}

/// Parses an unsigned decimal, rejecting anything that is not entirely digits.
std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
    if (text.empty()) return std::nullopt;

    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return std::nullopt;
        value = (value * 10) + static_cast<std::uint64_t>(character - '0');
    }
    return value;
}

/// Result of parsing the command line.
enum class ParseOutcome : std::uint8_t { Run, PrintedHelp, BadUsage };

ParseOutcome parseCommandLine(std::span<char*> arguments, Invocation& invocation) {
    const auto requireValue = [&](std::size_t& index, std::string_view option) -> const char* {
        if (index + 1 >= arguments.size()) {
            std::cerr << "enzan: option '" << option << "' needs a value\n";
            return nullptr;
        }
        return arguments[++index];
    };

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument(arguments[index]);

        if (argument == "-h" || argument == "--help") {
            std::cout << UsageText;
            return ParseOutcome::PrintedHelp;
        }
        if (argument == "-V" || argument == "--version") {
            printVersion();
            return ParseOutcome::PrintedHelp;
        }

        if (argument == "-t" || argument == "--top") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.topModule = value;
            continue;
        }

        if (argument == "-r" || argument == "--run") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            const std::optional<std::uint64_t> parsed = parseUnsigned(value);
            if (!parsed.has_value()) {
                std::cerr << "enzan: '" << value << "' is not a time in whole units\n";
                return ParseOutcome::BadUsage;
            }
            invocation.runUntil = parsed;
            continue;
        }

        if (argument == "--vcd") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.vcdPath = value;
            continue;
        }

        if (argument == "--vcd-scope") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.vcdScope = value;
            continue;
        }

        if (argument == "--timescale") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            const std::optional<std::int8_t> exponent = parseTimeUnit(value);
            if (!exponent.has_value()) {
                std::cerr << "enzan: '" << value
                          << "' is not a time unit; expected s, ms, us, ns, ps or fs\n";
                return ParseOutcome::BadUsage;
            }
            invocation.timeExponent = *exponent;
            continue;
        }

        if (argument == "--delta-limit") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            const std::optional<std::uint64_t> parsed = parseUnsigned(value);
            if (!parsed.has_value() || *parsed == 0 || *parsed > 0xFFFF'FFFFU) {
                std::cerr << "enzan: '" << value << "' is not a usable delta limit\n";
                return ParseOutcome::BadUsage;
            }
            invocation.deltaLimit = static_cast<std::uint32_t>(*parsed);
            continue;
        }

        if (argument == "-D" || argument == "--define") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.defines.emplace_back(value);
            continue;
        }

        if (argument == "-I" || argument == "--incdir") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.includeDirectories.emplace_back(value);
            continue;
        }

        if (argument == "--dump-stats") {
            invocation.dumpStatistics = true;
            continue;
        }

        if (argument.starts_with('-')) {
            std::cerr << "enzan: unrecognised option '" << argument << "'\n"
                      << "Try 'enzan --help' for the available options.\n";
            return ParseOutcome::BadUsage;
        }

        invocation.files.emplace_back(argument);
    }

    return ParseOutcome::Run;
}

#ifdef ENZAN_WITH_SYSTEMVERILOG

/// Analyses, elaborates, compiles and runs. Returns the process exit status.
ExitStatus simulate(const Invocation& invocation, enzan::SourceManager& sources,
                    enzan::DiagnosticEngine& diagnostics) {
    enzan::frontend::sv::Options frontendOptions;
    frontendOptions.files = invocation.files;
    frontendOptions.defines = invocation.defines;
    frontendOptions.includeDirectories = invocation.includeDirectories;
    if (!invocation.topModule.empty()) frontendOptions.topModules.push_back(invocation.topModule);

    const std::unique_ptr<enzan::eir::Design> design =
        enzan::frontend::sv::analyse(frontendOptions, sources, diagnostics);
    if (design == nullptr) return ExitStatus::Failure;

    enzan::elab::Elaborator elaborator(*design, diagnostics);
    const std::unique_ptr<enzan::elab::ElaboratedDesign> elaborated =
        elaborator.elaborate(invocation.topModule);
    if (elaborated == nullptr) return ExitStatus::Failure;

    enzan::codegen::Compiler compiler(diagnostics);
    const std::unique_ptr<enzan::codegen::CompiledDesign> compiled = compiler.compile(*design);
    if (compiled == nullptr) return ExitStatus::Failure;

    const enzan::runtime::Image image(*design, *elaborated, *compiled);

    enzan::runtime::KernelOptions kernelOptions;
    kernelOptions.timeScale = enzan::runtime::TimeScale(invocation.timeExponent);
    kernelOptions.deltaLimit = invocation.deltaLimit;

    enzan::runtime::Kernel kernel(image, diagnostics, kernelOptions);
    kernel.initialize();

    // The writer is created after `initialize` so that the values it dumps at time zero are
    // the ones the design actually starts from.
    std::unique_ptr<enzan::wavedb::VcdWriter> writer;
    if (!invocation.vcdPath.empty()) {
        writer = std::make_unique<enzan::wavedb::VcdWriter>(image, kernel.arena(),
                                                            kernelOptions.timeScale);
        writer->setScope(invocation.vcdScope);

        std::string error;
        if (!writer->open(invocation.vcdPath, error)) {
            diagnostics.report(enzan::DiagCode::CannotOpenFile, enzan::SourceLocation(),
                               invocation.vcdPath.string(), error);
            return ExitStatus::Failure;
        }
        kernel.addListener(*writer);
    }

    const enzan::runtime::RunStatus status =
        kernel.run(invocation.runUntil.value_or(enzan::runtime::NoTime));

    if (writer != nullptr) writer->close();

    if (invocation.dumpStatistics) {
        std::cout << "-- " << kernel.executedInstructions() << " instructions, "
                  << kernel.totalDeltaCycles() << " delta cycles, ended at "
                  << kernelOptions.timeScale.format(kernel.time()) << " ("
                  << enzan::runtime::toString(status) << ")\n";
    }

    if (status == enzan::runtime::RunStatus::Error || kernel.exitStatus() != 0
        || diagnostics.hasErrors()) {
        return ExitStatus::Failure;
    }
    return ExitStatus::Success;
}

#endif // ENZAN_WITH_SYSTEMVERILOG

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> rawArguments(argv, static_cast<std::size_t>(argc));
    const auto arguments = rawArguments.subspan(rawArguments.empty() ? 0 : 1);

    // No arguments is an error rather than a no-op: there is no default action, and silently
    // succeeding would make a script that forgot its arguments look as though it had run a
    // simulation.
    if (arguments.empty()) {
        std::cerr << UsageText;
        return static_cast<int>(ExitStatus::Usage);
    }

    Invocation invocation;
    switch (parseCommandLine(arguments, invocation)) {
    case ParseOutcome::PrintedHelp: return static_cast<int>(ExitStatus::Success);
    case ParseOutcome::BadUsage: return static_cast<int>(ExitStatus::Usage);
    case ParseOutcome::Run: break;
    }

    if (invocation.files.empty()) {
        std::cerr << "enzan: no source files were given\n"
                  << "Try 'enzan --help' for the available options.\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    enzan::SourceManager sources;
    enzan::DiagnosticEngine diagnostics(sources);
    enzan::TextDiagnosticConsumer consumer(sources, std::cerr);
    diagnostics.addConsumer(consumer);

#ifdef ENZAN_WITH_SYSTEMVERILOG
    return static_cast<int>(simulate(invocation, sources, diagnostics));
#else
    // Building without the frontend is a legitimate configuration — it is what the CI build
    // matrix uses — so it must fail with the reason rather than with a link error.
    diagnostics.report(enzan::DiagCode::NotYetImplemented, enzan::SourceLocation(),
                       "simulation in a build configured with -DENZAN_ENABLE_SYSTEMVERILOG=OFF");
    return static_cast<int>(ExitStatus::Failure);
#endif
}
