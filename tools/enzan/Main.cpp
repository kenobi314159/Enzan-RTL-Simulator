// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point for the headless simulation kernel.
//
// The kernel and the GUI are separate processes (DESIGN_GUIDE.md section 2.2). This
// executable must stay usable with no display attached, so nothing here may reach into
// src/gui or link Qt.
//
// The command line is deliberately minimal: enough to analyse, elaborate and run one design,
// or to serve it to a front end. Section 8 holds the rules it shares with `enzan-gui`.

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "common/Version.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eir/Library.h"
#include "session/FileList.h"
#include "session/Session.h"

#include <sys/wait.h>
#include <unistd.h>

// Simulating needs a frontend to analyse with, and either language's will do.
#if defined(ENZAN_WITH_SYSTEMVERILOG) || defined(ENZAN_WITH_VHDL)
#define ENZAN_WITH_FRONTEND
#endif

#ifdef ENZAN_WITH_FRONTEND
#include "ipc/KernelServer.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"
#endif

namespace {

/// Exit statuses. `Usage` is distinct from a simulation failure so that scripts can tell
/// "you invoked me wrongly" from "the design failed".
/// Exit statuses, distinct enough for a Makefile to branch on (DESIGN_GUIDE.md section 8.1).
///
/// `AnalysisFailed` and `SimulationFailed` are separate because a regression script that
/// cannot tell "the tool could not compile this" from "the testbench reported an error"
/// reports both as the same red square.
enum class ExitStatus : std::uint8_t {
    Success = 0,
    Usage = 1,
    AnalysisFailed = 2,
    SimulationFailed = 3,
};

/// What the command line asked to be done.
enum class Command : std::uint8_t {
    /// No subcommand: analyse and run in one process, which is what a small case wants.
    OneShot,
    Analyse,
    Elaborate,
    Run,
    LibCreate,
    LibList,
    LibClean,
};

/// A library named on the command line, with the name it is to be known by.
///
/// An empty `name` means the one the library carries, which is the usual case: the name lives
/// in the library so that the two ends of a build cannot disagree about it (8.2).
struct NamedLibrary {
    std::string name;
    std::filesystem::path path;
};

/// A file list and the rule its relative paths resolve by (8.3).
struct FileListRequest {
    std::filesystem::path path;
    enzan::session::FileListAnchor anchor = enzan::session::FileListAnchor::WorkingDirectory;
};

constexpr std::string_view UsageText =
    R"(Usage: enzan [command] [options] [arguments]

Enzan is an event-driven, mixed-language RTL simulator for SystemVerilog and VHDL.

Commands:
  analyse <sources...>    Analyse sources into design units.
  elaborate <top>         Elaborate and report; simulate nothing.
  run <top>               Elaborate and simulate.
  lib create <dir>        Create an analysis library.
  lib list <dir>          List what a library holds.
  lib clean <dir>         Remove a library's entries, keeping the library.

With no command, the sources given are analysed and run in one process, which is
what a small case wants. A source file called `run.sv` is still a source file: the
first word is a command only when it is one of the names above.

Sources:
  -f <list.f>             Read a file list. Relative paths inside it resolve
                          against the working directory.
  -F <list.f>             The same, resolving against the list's own directory.
      --context <file>    Analyse this so references resolve, without storing its
                          units. For a package shared across libraries.
      --dry-run           Print what the file lists expanded to, and exit.
      --depfile <file>    Write a make-style dependency file of every file read.
      --depfile-target <t>
                          The target those prerequisites attach to.
  -D, --define <NAME[=V]> Define a preprocessor macro. +define+ is also accepted.
  -I, --incdir <dir>      Add a directory to the `include search path. +incdir+
                          is also accepted.
  -G <NAME=VALUE>         Set a parameter of the top level, overriding its
                          default. -GNAME=VALUE is also accepted.

Libraries:
      --out-lib <dir>     The library analysis is written into, created when it
                          is absent and named after the directory.
      --in-lib <dir>      A library to resolve against; repeatable.
      --in-lib <name>=<dir>
                          The same, overriding the name the library carries.
      --name <n>          The name `lib create` gives a library.
      --work <name>       The library VHDL sources are analysed into, which they
                          name it by: `library neorv32;`. Default `work`.
      --std <1993|2000|2002|2008|2019>
                          The revision of IEEE 1076 VHDL sources are analysed
                          against, or its last two digits. Default 2008.
      --ieee-asserts <enable|disable>
                          Whether the IEEE packages' own warnings, numeric_std's
                          of metavalues and truncation, are reported. As GHDL's
                          option of the name; enabled by default.

Simulation:
  -t, --top <module>      Module to elaborate as the top level. `elaborate` and
                          `run` take it as their one positional word instead.
  -r, --run <time>        Stop after this many time units.
      --assert-level <note|warning|error|failure|none>
                          The severity at which a VHDL report or failed assertion
                          ends the run, as GHDL's option of the name. Default
                          `failure`; `none` never ends it.
      --vcd <file>        Write a VCD waveform to this file.
      --fst <file>        Write an FST waveform to this file. Far smaller on a
                          real design, and seekable. Both may be given at once.
      --wave-scope <path> Record only this subtree, for example /tb/u_dut.
      --timescale <unit>  Time unit for delays and reported times: s, ms, us, ns,
                          ps or fs. The default is the unit the libraries were
                          analysed in, and ps without any.
      --delta-limit <n>   Delta cycles allowed at one timestamp. Default 10000.
      --seed <n>          The seed random numbers are drawn from. Default 0.
      --dump-stats        Print instruction and delta counts when the run ends.
      --vpi <lib[:entry]> Load a VPI application, such as cocotb's; repeatable.
                          Without an entry, its vlog_startup_routines are run.
      --vhpi <lib[:entry]>
                          Load a VHPI application, such as cocotb's for a VHDL
                          toplevel; repeatable. Without an entry, its
                          vhpi_startup_routines are run.
  +<plusarg>              Passed to the design and to VPI applications.

Graphical front end:
      --gui               Hand the elaborated design to a window rather than
                          running here. Closing the window ends this kernel.
      --gui --attach <s>  Attach to a kernel already listening on socket s.
                          Closing that window leaves the kernel running.
      --listen <socket>   Elaborate and wait for a front end without starting
                          one, so the kernel outlives whatever attaches.
      --theme <file>      Colour the window from this Tcl theme script. The
                          default is $XDG_CONFIG_HOME/enzan-gui/theme.tcl.
      --write-theme <file>
                          Write the built-in theme out as a starting point for
                          one of your own, and exit.
      --wave-list <file>  Open the wave window with this signal list in it.
                          The window's Wave menu saves one.

  -h, --help              Print this message and exit.
  -V, --version           Print version and build information and exit.

Exit statuses: 0 success, 1 the command line was wrong, 2 analysis or elaboration
failed, 3 the simulation ran and the design failed.

The normative specification is DESIGN_GUIDE.md in the source tree.
)";

void printVersion() {
    std::cout << enzan::versionNotice("enzan");
}

/// Everything the command line can ask for.
struct Invocation {
    Command command = Command::OneShot;

    std::vector<std::filesystem::path> files;
    std::vector<FileListRequest> fileLists;
    std::vector<std::filesystem::path> contextFiles;

    std::filesystem::path outputLibrary;
    std::vector<NamedLibrary> inputLibraries;

    std::filesystem::path depfilePath;
    std::string depfileTarget;
    bool dryRun = false;
    bool useGui = false;
    std::filesystem::path attachPath;
    std::string libraryName;
    std::filesystem::path themePath;
    std::filesystem::path writeThemePath;

    /// A signal list the window starts with (DESIGN_GUIDE.md section 10.8).
    std::filesystem::path waveListPath;
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> includeDirectories;

    /// Top-level parameter overrides, each `NAME=VALUE`.
    std::vector<std::string> parameters;

    /// The library VHDL sources are analysed into, by the name they refer to it by.
    std::string workLibrary = "work";
    std::string vhdlStandard = "2008";
    bool ieeeWarnings = true;

    /// The severity, as a position of `severity_level`, at which a VHDL report ends the run; 4
    /// never does.
    std::uint8_t stopSeverity = 3;
    std::string topModule;
    std::optional<std::uint64_t> runUntil;
    std::filesystem::path vcdPath;
    std::filesystem::path fstPath;
    std::string waveformScope;
    std::filesystem::path listenPath;
    /// Given with `--timescale`; otherwise the libraries' unit, or the session's default.
    std::optional<std::int8_t> timeExponent;
    std::uint32_t deltaLimit = 10'000;
    std::uint32_t seed = 0;
    bool dumpStatistics = false;

    /// VPI and VHPI applications to load, each in the order given (DESIGN_GUIDE.md sections 9.1
    /// and 9.7).
    std::vector<std::string> vpiApplications;
    std::vector<std::string> vhpiApplications;

    /// The command line as it was given, which is what `vpi_get_vlog_info` reports.
    std::vector<std::string> commandLine;
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

/// Splits `+incdir+a+b` into its parts, dropping the empty ones a trailing `+` produces.
std::vector<std::string_view> splitPlusList(std::string_view token) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= token.size()) {
        const std::size_t end = std::min(token.find('+', start), token.size());
        if (end > start) parts.push_back(token.substr(start, end - start));
        start = end + 1;
    }
    return parts;
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

/// Recognises a leading subcommand, consuming it and any word it takes.
///
/// The first word is a subcommand only when it is one of these names, so a source file called
/// `run.sv` is still a source file (8.1).
std::size_t readCommand(std::span<char*> arguments, Invocation& invocation) {
    if (arguments.empty()) return 0;

    const std::string_view first(arguments[0]);
    if (first == "analyse" || first == "analyze") {
        invocation.command = Command::Analyse;
        return 1;
    }
    if (first == "elaborate") {
        invocation.command = Command::Elaborate;
        return 1;
    }
    if (first == "run") {
        invocation.command = Command::Run;
        return 1;
    }
    if (first != "lib") return 0;

    const std::string_view second = arguments.size() > 1 ? std::string_view(arguments[1]) : "";
    if (second == "create") {
        invocation.command = Command::LibCreate;
    } else if (second == "list") {
        invocation.command = Command::LibList;
    } else if (second == "clean") {
        invocation.command = Command::LibClean;
    } else {
        std::cerr << "enzan: `lib` takes create, list or clean\n";
        invocation.command = Command::OneShot;
        return 0;
    }
    return 2;
}

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

        if (argument == "--work") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.workLibrary = value;
            continue;
        }

        if (argument == "--std") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            static constexpr std::array<std::string_view, 10> Revisions{
                "1993", "93", "2000", "00", "2002", "02", "2008", "08", "2019", "19"};
            if (std::ranges::find(Revisions, std::string_view(value)) == Revisions.end()) {
                std::cerr << "enzan: '" << value
                          << "' is not a revision of VHDL: 1993, 2000, 2002, 2008 or 2019\n";
                return ParseOutcome::BadUsage;
            }
            invocation.vhdlStandard = value;
            continue;
        }

        if (argument == "--ieee-asserts") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            const std::string_view chosen = value;
            if (chosen != "enable" && chosen != "disable") {
                std::cerr << "enzan: '" << chosen << "' is not `enable` or `disable`\n";
                return ParseOutcome::BadUsage;
            }
            invocation.ieeeWarnings = chosen == "enable";
            continue;
        }

        if (argument == "--assert-level") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            static constexpr std::array<std::string_view, 5> Severities{"note", "warning", "error",
                                                                        "failure", "none"};
            const auto* const found = std::ranges::find(Severities, std::string_view(value));
            if (found == Severities.end()) {
                std::cerr << "enzan: '" << value
                          << "' is not a severity: note, warning, error, failure or none\n";
                return ParseOutcome::BadUsage;
            }
            invocation.stopSeverity = static_cast<std::uint8_t>(found - Severities.begin());
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

        if (argument == "--fst") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.fstPath = value;
            continue;
        }

        // `--vcd-scope` stays as an alias: the filter was never specific to VCD, and a script
        // written against the earlier name should keep working.
        if (argument == "--wave-scope" || argument == "--vcd-scope") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.waveformScope = value;
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
            invocation.timeExponent = exponent;
            continue;
        }

        if (argument == "--listen") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.listenPath = value;
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

        if (argument == "--seed") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            const std::optional<std::uint64_t> parsed = parseUnsigned(value);
            if (!parsed.has_value() || *parsed > 0xFFFF'FFFFU) {
                std::cerr << "enzan: '" << value << "' is not a seed from 0 to 4294967295\n";
                return ParseOutcome::BadUsage;
            }
            invocation.seed = static_cast<std::uint32_t>(*parsed);
            continue;
        }

        if (argument == "-D" || argument == "--define") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.defines.emplace_back(value);
            continue;
        }

        // `-G NAME=VALUE`, or `-GNAME=VALUE` as Verilator spells it and as cocotb's makefiles
        // pass it.
        if (argument == "-G") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.parameters.emplace_back(value);
            continue;
        }
        if (argument.starts_with("-G") && argument.size() > 2) {
            invocation.parameters.emplace_back(argument.substr(2));
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

        if (argument == "--vpi") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.vpiApplications.emplace_back(value);
            continue;
        }

        if (argument == "--vhpi") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
#ifdef ENZAN_WITH_VHDL
            invocation.vhpiApplications.emplace_back(value);
            continue;
#else
            std::cerr << "enzan: '--vhpi' needs the VHDL frontend, which this build leaves out\n";
            return ParseOutcome::BadUsage;
#endif
        }

        if (argument == "--out-lib") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.outputLibrary = value;
            continue;
        }

        if (argument == "--in-lib") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;

            // `<name>=<dir>` overrides the name the library carries; a bare path uses it.
            const std::string_view text(value);
            const std::size_t equals = text.find('=');
            if (equals == std::string_view::npos) {
                invocation.inputLibraries.push_back({.name = {}, .path = text});
            } else {
                invocation.inputLibraries.push_back(
                    {.name = std::string(text.substr(0, equals)), .path = text.substr(equals + 1)});
            }
            continue;
        }

        if (argument == "--context") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.contextFiles.emplace_back(value);
            continue;
        }

        if (argument == "-f" || argument == "-F") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.fileLists.push_back(
                {.path = value,
                 .anchor = argument == "-F" ? enzan::session::FileListAnchor::ListDirectory
                                            : enzan::session::FileListAnchor::WorkingDirectory});
            continue;
        }

        if (argument == "--depfile") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.depfilePath = value;
            continue;
        }

        if (argument == "--depfile-target") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.depfileTarget = value;
            continue;
        }

        if (argument == "--attach") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.attachPath = value;
            continue;
        }

        if (argument == "--name") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.libraryName = value;
            continue;
        }

        if (argument == "--theme") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.themePath = value;
            continue;
        }

        if (argument == "--write-theme") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.writeThemePath = value;
            continue;
        }

        if (argument == "--wave-list") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.waveListPath = value;
            continue;
        }

        if (argument == "--dry-run") {
            invocation.dryRun = true;
            continue;
        }

        if (argument == "--gui") {
            invocation.useGui = true;
            continue;
        }

        if (argument.starts_with("+incdir+")) {
            for (const std::string_view part : splitPlusList(argument.substr(8)))
                invocation.includeDirectories.emplace_back(part);
            continue;
        }

        if (argument.starts_with("+define+")) {
            for (const std::string_view part : splitPlusList(argument.substr(8)))
                invocation.defines.emplace_back(part);
            continue;
        }

        // `-sv` asks for the only language this build reads, so it is accepted and dropped
        // exactly as the file-list reader treats it.
        if (argument == "-sv") continue;

        // Any other `+` word is a plusarg. It belongs to the design and its VPI applications,
        // which read it from the command line `vpi_get_vlog_info` reports (section 9.1), so it
        // is accepted here and needs nothing further. The analysis options above are the
        // exception, and `+libext+` stays refused below because it names a file-list feature
        // that this command line does not have.
        if (argument.starts_with('+') && !argument.starts_with("+libext+")) continue;

        if (argument.starts_with('-') || argument.starts_with('+')) {
            std::cerr << "enzan: unrecognised option '" << argument << "'\n"
                      << "Try 'enzan --help' for the available options.\n";
            return ParseOutcome::BadUsage;
        }

        invocation.files.emplace_back(argument);
    }

    return ParseOutcome::Run;
}

/// The front end executable, beside this one first so that a build tree runs its own.
std::filesystem::path frontEndProgram() {
    std::error_code failure;
    std::filesystem::path beside =
        std::filesystem::read_symlink("/proc/self/exe", failure).parent_path() / "enzan-gui";
    if (!failure && std::filesystem::exists(beside, failure)) return beside;
    return "enzan-gui";
}

#ifdef ENZAN_WITH_FRONTEND

/// The socket a signal handler has to remove, and nothing else.
///
/// A plain buffer rather than a `std::string`, because a handler may only call what POSIX lists
/// as async-signal-safe and may not touch an allocator. Written once before any handler is
/// installed and never again.
std::array<char, 4096> signalSocketPath{};

/// Removes the socket and lets the signal take its course.
///
/// Without this a kernel stopped with Ctrl-C or `kill` leaves its socket behind: a signal that
/// is not handled ends the process without running a destructor, and the listener's own cleanup
/// never happens. `unlink` is async-signal-safe; re-raising after restoring the default gives
/// the shell the exit status it expects rather than inventing one.
extern "C" void removeSocketOnSignal(int signalNumber) {
    if (signalSocketPath[0] != '\0') ::unlink(signalSocketPath.data());

    ::signal(signalNumber, SIG_DFL);
    ::raise(signalNumber);
}

/// Arranges for `socketPath` to be removed if this process is signalled.
///
/// **`SIGKILL` cannot be included and cannot be worked around**: it is not deliverable to a
/// handler by design, so a kernel killed with it always leaves its socket on disk. That is why
/// binding probes an existing path and reclaims one nothing is listening on (`Listener::listen`)
/// — the leftover costs nothing because the next kernel takes it back.
void removeSocketWhenSignalled(const std::filesystem::path& socketPath) {
    const std::string text = socketPath.string();
    if (text.size() + 1 > signalSocketPath.size()) return;

    std::ranges::copy(text, signalSocketPath.begin());
    signalSocketPath[text.size()] = '\0';

    for (const int signalNumber : {SIGINT, SIGTERM, SIGHUP}) {
        ::signal(signalNumber, removeSocketOnSignal);
    }
}

#endif // ENZAN_WITH_FRONTEND

/// The window's own options, as the front end spells them.
///
/// `--gui` is the only way in (8.4), so anything the window needs has to travel through here.
/// Its theme and the signal list it starts with are what a user configures about it, and
/// leaving those reachable only by running the front end directly would make it a second
/// program to learn after all.
std::vector<std::string> frontEndOptions(const Invocation& invocation) {
    std::vector<std::string> options;
    if (!invocation.themePath.empty()) {
        options.emplace_back("--theme");
        options.push_back(invocation.themePath.string());
    }
    if (!invocation.writeThemePath.empty()) {
        options.emplace_back("--write-theme");
        options.push_back(invocation.writeThemePath.string());
    }
    if (!invocation.waveListPath.empty()) {
        options.emplace_back("--wave-list");
        options.push_back(invocation.waveListPath.string());
    }
    return options;
}

/// Runs the front end with `options` and nothing else, and never returns.
///
/// For `--write-theme`, which writes a file and exits: there is no simulation in it, so a kernel
/// would be a process started to do nothing.
[[noreturn]] void becomeFrontEnd(const std::vector<std::string>& options) {
    const std::filesystem::path program = frontEndProgram();

    std::vector<std::string> owned = options;
    std::string name = program.string();

    std::vector<char*> argv;
    argv.push_back(name.data());
    for (std::string& option : owned) argv.push_back(option.data());
    argv.push_back(nullptr);

    ::execvp(name.c_str(), argv.data());

    std::cerr << "enzan: could not start the graphical front end '" << name
              << "'; this build may have been configured without it (-DENZAN_BUILD_GUI=ON)\n";
    std::exit(static_cast<int>(ExitStatus::Usage));
}

#ifdef ENZAN_WITH_FRONTEND

/// Starts a window attached to `socketPath`, and returns its process id or -1.
///
/// `--gui` is an option of `enzan` rather than a second program to learn, but the two remain
/// two processes and the kernel binary links no Qt: a headless server and a CI runner must not
/// install a GUI toolkit to run a batch simulation (2.2). The kernel is what parses the command
/// line, analyses and elaborates; the window only ever attaches, which is why it needs to know
/// nothing about libraries or file lists.
::pid_t startFrontEnd(const Invocation& invocation, const std::filesystem::path& socketPath) {
    const ::pid_t child = ::fork();
    if (child != 0) return child;

    std::vector<std::string> options = frontEndOptions(invocation);
    options.emplace_back("--attach");
    options.push_back(socketPath.string());

    const std::filesystem::path program = frontEndProgram();
    std::string name = program.string();

    std::vector<char*> argv;
    argv.push_back(name.data());
    for (std::string& option : options) argv.push_back(option.data());
    argv.push_back(nullptr);

    ::execvp(name.c_str(), argv.data());

    std::cerr << "enzan: could not start the graphical front end '" << name
              << "'; this build may have been configured without it (-DENZAN_BUILD_GUI=ON)\n";
    std::_Exit(static_cast<int>(ExitStatus::Usage));
}

#endif // ENZAN_WITH_FRONTEND

/// Replaces this process with a window attached to an already-listening kernel.
///
/// `--gui --attach` starts no kernel, so there is nothing here to keep: closing that window
/// leaves the kernel it found running, which is the lifetime 8.4 gives this spelling.
[[noreturn]] void becomeAttachedFrontEnd(const Invocation& invocation) {
    std::vector<std::string> options = frontEndOptions(invocation);
    options.emplace_back("--attach");
    options.push_back(invocation.attachPath.string());
    becomeFrontEnd(options);
}

/// Expands the file lists and the sources named directly into one analysis request.
///
/// A list's own options join the ones given on the command line rather than replacing them,
/// which is what a design that ships a `.f` naming its include directories expects.
bool collectSources(const Invocation& invocation, enzan::SourceManager& sources,
                    enzan::DiagnosticEngine& diagnostics,
                    enzan::session::Session::AnalysisRequest& request) {
    for (const FileListRequest& list : invocation.fileLists) {
        enzan::session::FileList expanded;
        if (!readFileList(list.path, sources, diagnostics, expanded, list.anchor)) return false;

        for (const std::filesystem::path& file : expanded.files) request.files.push_back(file);
        for (const std::string& define : expanded.defines) request.defines.push_back(define);
        for (const std::filesystem::path& directory : expanded.includeDirectories)
            request.includeDirectories.push_back(directory);
    }

    for (const std::filesystem::path& file : invocation.files) request.files.push_back(file);
    for (const std::string& define : invocation.defines) request.defines.push_back(define);
    for (const std::filesystem::path& directory : invocation.includeDirectories)
        request.includeDirectories.push_back(directory);
    for (const std::string& parameter : invocation.parameters)
        request.parameters.push_back(parameter);

    request.contextFiles = invocation.contextFiles;
    request.top = invocation.topModule;
    request.vhdlWork = invocation.workLibrary;
    request.ieeeWarnings = invocation.ieeeWarnings;
    request.vhdlStandard = invocation.vhdlStandard;
    return true;
}

#ifdef ENZAN_WITH_FRONTEND

/// Builds the session every path shares: analysis and elaboration, once.
///
/// One pipeline for the batch run, the served kernel and the script. Three copies of it was
/// where the differences between them started: a `--listen` that resolved a file differently
/// differently from another, or a batch run that put the waveform somewhere a served one did not.
/// Opens the libraries this invocation writes into and reads from, and settles the time unit,
/// which the libraries fix when they hold units (8.2).
bool openLibraries(const Invocation& invocation, enzan::session::Session& session) {
    if (!invocation.outputLibrary.empty() && !session.openOutputLibrary(invocation.outputLibrary))
        return false;

    for (const NamedLibrary& input : invocation.inputLibraries) {
        if (!session.addInputLibrary(input.path, input.name)) return false;
    }

    std::optional<enzan::runtime::TimeScale> requested;
    if (invocation.timeExponent.has_value())
        requested = enzan::runtime::TimeScale(*invocation.timeExponent);
    return session.chooseTimeScale(requested);
}

/// Analyses, without elaborating. Returns false having reported.
bool analyseOnly(const Invocation& invocation, enzan::session::Session& session,
                 enzan::SourceManager& sources, enzan::DiagnosticEngine& diagnostics) {
    if (!openLibraries(invocation, session)) return false;

    enzan::session::Session::AnalysisRequest request;
    if (!collectSources(invocation, sources, diagnostics, request)) return false;

    // An input library's own sources supply the declarations this analysis instantiates, which
    // is what lets those units be instantiated with whatever parameters are wanted here rather
    // than only the ones the library stored (8.2).
    request.libraryFiles = session.inputLibrarySources();

    // Ahead of this analysis's own, so that a library's own include path and macros are what
    // its sources are read with and anything named here can still override.
    std::vector<std::filesystem::path> includes = session.inputLibraryIncludeDirectories();
    for (const std::filesystem::path& directory : request.includeDirectories)
        includes.push_back(directory);
    request.includeDirectories = std::move(includes);

    std::vector<std::string> defines = session.inputLibraryDefines();
    for (const std::string& define : request.defines) defines.push_back(define);
    request.defines = std::move(defines);

    return session.analyse(request);
}

bool prepare(const Invocation& invocation, enzan::session::Session& session,
             enzan::SourceManager& sources, enzan::DiagnosticEngine& diagnostics) {
    // What only the kernel reads, set whether or not this run analyses anything: a run from
    // libraries alone skips analysis, and a seed or a delta limit given to it would otherwise be
    // dropped without a word.
    session.setDeltaLimit(invocation.deltaLimit);
    session.setStopSeverity(invocation.stopSeverity);
    session.setSeed(invocation.seed);

    // A run given nothing but libraries has no sources of its own to analyse, and elaboration
    // then works from what the libraries hold.
    const bool hasSources = !invocation.files.empty() || !invocation.fileLists.empty();
    if (hasSources && !analyseOnly(invocation, session, sources, diagnostics)) return false;
    if (!hasSources && !openLibraries(invocation, session)) return false;

    if (!invocation.waveformScope.empty() && !session.addWaveformScope(invocation.waveformScope))
        return false;

    return session.elaborate(invocation.topModule);
}

/// Serves an elaborated session to a graphical front end instead of running it here.
///
/// The client drives the run, so nothing happens until one attaches, and the kernel survives
/// its departure: section 2.2 requires a GUI to be able to detach from a long simulation on a
/// compute server without ending it.
/// Serves until the client is finished, or until a window this process started has closed.
///
/// `serve()` returns only when a client asks the simulation to stop, which is right for
/// `--listen`: a kernel that outlives its window is the whole point of that spelling. With
/// `--gui` the kernel exists *for* the window it started, so its closing ends the kernel too
/// (8.4). Stepping rather than serving is what lets both be true of one server.
int serveUntilDone(enzan::ipc::KernelServer& server, ::pid_t window) {
    while (server.step(200)) {
        if (window <= 0) continue;

        int status = 0;
        if (::waitpid(window, &status, WNOHANG) == window) break;
    }
    return server.exitStatus();
}

ExitStatus serve(const Invocation& invocation, const std::filesystem::path& socketPath,
                 enzan::session::Session& session, enzan::DiagnosticEngine& diagnostics) {
    enzan::ipc::ServerOptions serverOptions;
    serverOptions.kernelVersion = enzan::versionLine();
    serverOptions.timeScale = session.timeScale();
    serverOptions.deltaLimit = invocation.deltaLimit;
    serverOptions.waveformScope = invocation.waveformScope;

    enzan::ipc::KernelServer server(session, diagnostics, serverOptions);
    server.start();

    std::string error;
    if (!server.listen(socketPath, error)) {
        diagnostics.report(enzan::DiagCode::CannotOpenFile, enzan::SourceLocation(),
                           socketPath.string(), error);
        return ExitStatus::AnalysisFailed;
    }

    removeSocketWhenSignalled(server.socketPath());

    ::pid_t window = -1;
    if (invocation.useGui) {
        window = startFrontEnd(invocation, server.socketPath());
        if (window < 0) {
            std::cerr << "enzan: the graphical front end could not be started\n";
            return ExitStatus::Usage;
        }
    } else {
        // Printed rather than logged, because it is the one thing the user needs next and they
        // are looking at this terminal. Not printed under --gui, where the socket is an
        // implementation detail the user was deliberately not made to think about.
        std::cout << "-- listening on " << server.socketPath().string() << "\n"
                  << "-- attach with: enzan --gui --attach " << server.socketPath().string() << '\n'
                  << std::flush;
    }

    // After the window has started, so that the window's own output is not captured into the
    // transcript it shows. A capture that cannot be set up costs the transcript what the design's
    // applications print, and nothing else: the run goes ahead.
    if (!server.captureOutput(error)) {
        std::cerr << "enzan: what VPI applications print will not appear in the window's "
                     "transcript: "
                  << error << '\n';
    }

    const int status = serveUntilDone(server, window);
    if (status != 0) return ExitStatus::SimulationFailed;
    if (diagnostics.hasErrors()) return ExitStatus::AnalysisFailed;
    return ExitStatus::Success;
}

/// Analyses, elaborates and runs. Returns the process exit status.
ExitStatus simulate(const Invocation& invocation, enzan::SourceManager& sources,
                    enzan::DiagnosticEngine& diagnostics) {
    enzan::session::Session session(sources, diagnostics);

    // Before analysis, so that a library that cannot be loaded is reported before a long
    // analysis rather than after it (section 9.1).
    for (const std::string& application : invocation.vpiApplications) {
        if (!session.loadVpiApplication(application, invocation.commandLine))
            return ExitStatus::AnalysisFailed;
    }
    for (const std::string& application : invocation.vhpiApplications) {
        if (!session.loadVhpiApplication(application, invocation.commandLine))
            return ExitStatus::AnalysisFailed;
    }

    if (!prepare(invocation, session, sources, diagnostics)) return ExitStatus::AnalysisFailed;

    // Opened before serving as well as before running: a served session records into the live
    // database *and* into any writer that was asked for, so that someone debugging in a window
    // who wants a file for later does not have to run the design again (8.4).
    if (!invocation.vcdPath.empty() && !session.openVcd(invocation.vcdPath))
        return ExitStatus::AnalysisFailed;
    if (!invocation.fstPath.empty() && !session.openFst(invocation.fstPath))
        return ExitStatus::AnalysisFailed;

    if (invocation.useGui) {
        // A socket in the user's runtime directory: per-user by construction, which is the
        // access control a Unix domain socket gets. Its name is not the user's business.
        const char* runtime = std::getenv("XDG_RUNTIME_DIR"); // NOLINT(concurrency-mt-unsafe)
        const std::filesystem::path directory = runtime != nullptr
                                                    ? std::filesystem::path(runtime)
                                                    : std::filesystem::temp_directory_path();
        const std::filesystem::path socketPath =
            directory / ("enzan-" + std::to_string(::getpid()) + ".sock");
        std::error_code ignored;
        std::filesystem::remove(socketPath, ignored);

        return serve(invocation, socketPath, session, diagnostics);
    }

    if (!invocation.listenPath.empty())
        return serve(invocation, invocation.listenPath, session, diagnostics);

    const enzan::runtime::RunStatus status = session.run(invocation.runUntil);

    // Closed before the statistics are printed, so that a writer failing to finish its file says
    // so before the run looks successful.
    session.closeWaveforms();

    if (invocation.dumpStatistics) {
        const enzan::runtime::Kernel* kernel = session.simulationKernel();
        std::cout << "-- " << kernel->executedInstructions() << " instructions, "
                  << kernel->totalDeltaCycles() << " delta cycles, ended at "
                  << session.timeScale().format(session.time()) << " ("
                  << enzan::runtime::toString(status) << ")\n";
    }

    // Analysis and elaboration have succeeded by here, so an error reported since — an `$error`,
    // a failed assertion — is the design failing in simulation, which is status 3 (8.1).
    if (status == enzan::runtime::RunStatus::Error || session.exitStatus() != 0
        || diagnostics.hasErrors())
        return ExitStatus::SimulationFailed;
    return ExitStatus::Success;
}

#endif // ENZAN_WITH_FRONTEND

#ifdef ENZAN_WITH_FRONTEND

/// Writes a `make`-style dependency file naming every file the analysis read (8.3).
///
/// One target and its prerequisites, continued across lines the way a compiler writes them.
/// Nothing is written when the analysis came from the library: see `Session::filesRead`.
bool writeDependencyFile(const Invocation& invocation, const enzan::session::Session& session,
                         enzan::DiagnosticEngine& diagnostics) {
    const std::span<const std::filesystem::path> read = session.filesRead();
    if (read.empty()) return true;

    // The target has to be given, because analysis has no single output file for it to be
    // inferred from; the output library stands in when it is not.
    std::string target = invocation.depfileTarget;
    if (target.empty()) target = invocation.outputLibrary.string();

    std::string contents = target;
    contents += ':';
    for (const std::filesystem::path& file : read) {
        contents += " \\\n  ";
        contents += file.string();
    }
    contents += '\n';

    // A phony target per prerequisite, so that deleting a header does not stop the build with
    // "no rule to make target" — the same trick a compiler's `-MP` plays, and for the same
    // reason: the build should rebuild, not refuse to run.
    for (const std::filesystem::path& file : read) {
        contents += '\n';
        contents += file.string();
        contents += ":\n";
    }

    std::ofstream out(invocation.depfilePath, std::ios::trunc);
    out << contents;
    if (out) return true;

    diagnostics.report(enzan::DiagCode::CannotOpenFile, enzan::SourceLocation(),
                       invocation.depfilePath.string(), "the dependency file could not be written");
    return false;
}

#endif // ENZAN_WITH_FRONTEND

/// `enzan lib create|list|clean`.
ExitStatus runLibCommand(const Invocation& invocation, enzan::DiagnosticEngine& diagnostics) {
    if (invocation.files.size() != 1) {
        std::cerr << "enzan: `lib` needs exactly one library directory\n";
        return ExitStatus::Usage;
    }
    const std::filesystem::path& path = invocation.files.front();

    if (invocation.command == Command::LibCreate) {
        enzan::eir::Library library;
        if (!library.create(path, invocation.libraryName, diagnostics))
            return ExitStatus::AnalysisFailed;

        std::cout << "-- created library '" << library.name() << "' in " << path.string() << '\n';
        return ExitStatus::Success;
    }

    enzan::eir::Library library;
    if (!library.open(path, diagnostics)) return ExitStatus::AnalysisFailed;

    if (invocation.command == Command::LibList) {
        const std::vector<std::string> units = library.unitNames();
        std::cout << "library '" << library.name() << "' in " << path.string() << ", "
                  << units.size() << (units.size() == 1 ? " unit\n" : " units\n");
        for (const std::string& unit : units) std::cout << "  " << unit << '\n';
        return ExitStatus::Success;
    }

    // Clean: the entries go, the library and its name stay, so a Makefile that created it once
    // does not have to create it again.
    std::error_code failure;
    std::size_t removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path, failure)) {
        if (entry.path().extension() != ".eirlib") continue;
        if (std::filesystem::remove(entry.path(), failure)) ++removed;
    }
    std::cout << "-- removed " << removed << (removed == 1 ? " entry\n" : " entries\n");
    return ExitStatus::Success;
}

/// Prints what the file lists expanded to, in order, and nothing else (`--dry-run`, 8.3).
ExitStatus printExpandedSources(const Invocation& invocation, enzan::SourceManager& sources,
                                enzan::DiagnosticEngine& diagnostics) {
    enzan::session::Session::AnalysisRequest request;
    if (!collectSources(invocation, sources, diagnostics, request))
        return ExitStatus::AnalysisFailed;

    for (const std::filesystem::path& file : request.contextFiles)
        std::cout << file.string() << '\n';
    for (const std::filesystem::path& file : request.files) std::cout << file.string() << '\n';
    return ExitStatus::Success;
}

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
    invocation.commandLine.assign(rawArguments.begin(), rawArguments.end());
    const std::size_t consumed = readCommand(arguments, invocation);
    switch (parseCommandLine(arguments.subspan(consumed), invocation)) {
    case ParseOutcome::PrintedHelp: return static_cast<int>(ExitStatus::Success);
    case ParseOutcome::BadUsage: return static_cast<int>(ExitStatus::Usage);
    case ParseOutcome::Run: break;
    }

    // `elaborate` and `run` take the top level as their one positional word; everything else
    // reads its positional words as sources.
    if (invocation.command == Command::Elaborate || invocation.command == Command::Run) {
        if (invocation.files.size() > 1) {
            std::cerr << "enzan: `" << (invocation.command == Command::Run ? "run" : "elaborate")
                      << "` takes one top level; name sources with `analyse`, a file list or "
                         "a library\n";
            return static_cast<int>(ExitStatus::Usage);
        }
        if (!invocation.files.empty()) {
            invocation.topModule = invocation.files.front().string();
            invocation.files.clear();
        }
    }

    if (!invocation.writeThemePath.empty()) becomeFrontEnd(frontEndOptions(invocation));

    // An application observes a simulation, and these commands run none (section 9.1).
    if ((!invocation.vpiApplications.empty() || !invocation.vhpiApplications.empty())
        && invocation.command != Command::Run && invocation.command != Command::OneShot) {
        std::cerr << "enzan: '--vpi' and '--vhpi' need a simulation to load into; give them to "
                     "`run` or to the one-shot form\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    if (!invocation.themePath.empty() && !invocation.useGui) {
        std::cerr << "enzan: '--theme' needs '--gui'; it colours a window, and a headless run "
                     "has none\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    if (!invocation.waveListPath.empty() && !invocation.useGui) {
        std::cerr << "enzan: '--wave-list' needs '--gui'; it fills a wave window, and a headless "
                     "run has none. What a batch run records is chosen with --wave-scope\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    // `--attach` only means anything to a window: there is nothing for a headless kernel to
    // attach *to*, and silently ignoring it would leave a user watching a second simulation
    // start while they waited to be shown the one they meant.
    if (!invocation.attachPath.empty() && !invocation.useGui) {
        std::cerr << "enzan: '--attach' needs '--gui'; it is a window that attaches to a "
                     "running kernel, and a kernel has nothing to attach to\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    // Before the check for sources, because attaching needs none: the kernel is somewhere else
    // and has already analysed and elaborated whatever it is serving.
    if (invocation.useGui && !invocation.attachPath.empty()) becomeAttachedFrontEnd(invocation);

    const bool needsSources =
        invocation.command == Command::OneShot || invocation.command == Command::Analyse;
    if (needsSources && invocation.files.empty() && invocation.fileLists.empty()) {
        std::cerr << "enzan: no source files were given\n"
                  << "Try 'enzan --help' for the available options.\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    // A macro, an include directory and a parameter override apply while sources are analysed
    // (8.2). A run from libraries alone analyses nothing, so it would run what the libraries hold
    // as if the option had been taken.
    if (invocation.files.empty() && invocation.fileLists.empty()) {
        std::vector<std::string_view> analysisOptions;
        if (!invocation.defines.empty()) analysisOptions.emplace_back("'-D'");
        if (!invocation.includeDirectories.empty()) analysisOptions.emplace_back("'-I'");
        if (!invocation.parameters.empty()) analysisOptions.emplace_back("'-G'");
        if (!analysisOptions.empty()) {
            std::cerr << "enzan: ";
            for (std::size_t index = 0; index < analysisOptions.size(); ++index) {
                if (index != 0) std::cerr << (index + 1 == analysisOptions.size() ? " and " : ", ");
                std::cerr << analysisOptions[index];
            }
            std::cerr << (analysisOptions.size() == 1 ? " applies" : " apply")
                      << " while sources are analysed, and this analyses none; give "
                      << (analysisOptions.size() == 1 ? "it" : "them")
                      << " to the analysis that stores the top level\n";
            return static_cast<int>(ExitStatus::Usage);
        }
    }

    if (invocation.useGui && !invocation.listenPath.empty()) {
        std::cerr << "enzan: '--listen' cannot be combined with '--gui'; --gui starts a window "
                     "on a socket of its own, and --listen is for a kernel that outlives "
                     "whatever attaches to it\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    if (invocation.useGui && invocation.runUntil.has_value()) {
        std::cerr << "enzan: '--run' cannot be combined with '--gui'; the window drives the "
                     "run, and a design run before it attached would be put back to time zero "
                     "rather than shown\n";
        return static_cast<int>(ExitStatus::Usage);
    }

    // Refused rather than ignored. A served session is driven entirely by its client, and
    // silently dropping a `--run` or a `--vcd` the user asked for would be worse than saying
    // that this combination is not the one to reach for.
    if (!invocation.listenPath.empty()) {
        if (invocation.runUntil.has_value()) {
            std::cerr << "enzan: '--run' and '--listen' cannot be combined; a served session "
                         "is run from the attached client\n";
            return static_cast<int>(ExitStatus::Usage);
        }
    }

    enzan::SourceManager sources;
    enzan::DiagnosticEngine diagnostics(sources);
    enzan::TextDiagnosticConsumer consumer(sources, std::cerr);
    diagnostics.addConsumer(consumer);

    if (invocation.command == Command::LibCreate || invocation.command == Command::LibList
        || invocation.command == Command::LibClean) {
        return static_cast<int>(runLibCommand(invocation, diagnostics));
    }

    if (invocation.dryRun) {
        return static_cast<int>(printExpandedSources(invocation, sources, diagnostics));
    }

#ifdef ENZAN_WITH_FRONTEND
    if (invocation.command == Command::Analyse) {
        enzan::session::Session session(sources, diagnostics);
        const bool analysed = analyseOnly(invocation, session, sources, diagnostics);

        // Written even when the analysis failed, so that fixing the header it failed on
        // rebuilds rather than being invisible to the build.
        if (!invocation.depfilePath.empty()
            && !writeDependencyFile(invocation, session, diagnostics)) {
            return static_cast<int>(ExitStatus::AnalysisFailed);
        }
        if (!analysed) return static_cast<int>(ExitStatus::AnalysisFailed);
        return static_cast<int>(ExitStatus::Success);
    }

    if (invocation.command == Command::Elaborate) {
        enzan::session::Session session(sources, diagnostics);
        if (!prepare(invocation, session, sources, diagnostics))
            return static_cast<int>(ExitStatus::AnalysisFailed);
        return static_cast<int>(ExitStatus::Success);
    }

    return static_cast<int>(simulate(invocation, sources, diagnostics));
#else
    // Building without the frontend is a legitimate configuration — it is what the CI build
    // matrix uses — so it must fail with the reason rather than with a link error.
    diagnostics.report(enzan::DiagCode::NotYetImplemented, enzan::SourceLocation(),
                       "simulation in a build configured with -DENZAN_ENABLE_SYSTEMVERILOG=OFF "
                       "and -DENZAN_ENABLE_VHDL=OFF");
    return static_cast<int>(ExitStatus::AnalysisFailed);
#endif
}
