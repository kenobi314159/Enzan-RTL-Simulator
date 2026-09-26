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

#include "control/Commands.h"
#include "control/Interpreter.h"
#include "control/Session.h"

#include <unistd.h>

#ifdef ENZAN_WITH_SYSTEMVERILOG
#include "ipc/KernelServer.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"
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
      --fst <file>        Write an FST waveform to this file. Far smaller than a VCD on a
                          real design, and seekable, so prefer it unless you need to diff or
                          read the file yourself. Both may be given at once.
      --wave-scope <path> Record only this subtree, for example /tb/u_dut.
      --timescale <unit>  Time unit used for delays and reported times: s, ms, us, ns,
                          ps or fs. The default is ps.
      --delta-limit <n>   Delta cycles allowed at one timestamp before the run is
                          abandoned. The default is 10000.
      --listen <socket>   Elaborate the design and then wait on this Unix domain socket for
                          a graphical front end to attach, instead of running immediately.
                          The attached client drives the run, and the kernel keeps running
                          when it detaches, so a long simulation can be looked in on and
                          left alone again. Attach with `enzan-gui --attach <socket>`.
  -D, --define <NAME[=V]> Define a preprocessor macro.
  -I, --incdir <dir>      Add a directory to the `include search path.
      --dump-stats        Print instruction and delta counts when the run ends.

Control language:
      --do <script>       Run a Tcl script and exit with what `quit` asked for. The script
                          drives everything: `vlog`, `vsim`, `run`, `examine` and the rest of
                          the vocabulary in DESIGN_GUIDE.md section 8.1. Anything else on the
                          command line is passed to the script as $argv.
  -i, --interactive       Read commands from the standard input. Combined with --do, the
                          script runs first and the prompt follows it.

The control language is the whole interface when either of those is given, so the options
above are refused alongside them rather than quietly becoming a second way to say the same
thing. `vlog design.sv`, `vsim tb`, `run -all` is the shortest script that does what the
options above do.

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
    std::filesystem::path fstPath;
    std::string waveformScope;
    std::filesystem::path listenPath;
    std::filesystem::path scriptPath;
    std::int8_t timeExponent = -12;
    std::uint32_t deltaLimit = 10'000;
    bool dumpStatistics = false;
    bool interactive = false;
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
            invocation.timeExponent = *exponent;
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

        if (argument == "--do") {
            const char* value = requireValue(index, argument);
            if (value == nullptr) return ParseOutcome::BadUsage;
            invocation.scriptPath = value;
            continue;
        }

        if (argument == "-i" || argument == "--interactive") {
            invocation.interactive = true;
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

/// Builds the session every path shares: analysis and elaboration, once.
///
/// One pipeline for the batch run, the served kernel and the script. Three copies of it was
/// where the differences between them started: a `--listen` that resolved a file differently
/// from a `vlog`, or a batch run that put the waveform somewhere a served one did not.
bool prepare(const Invocation& invocation, enzan::control::Session& session,
             enzan::DiagnosticEngine& diagnostics) {
    session.setTimeScale(enzan::runtime::TimeScale(invocation.timeExponent));
    session.setDeltaLimit(invocation.deltaLimit);

    enzan::control::Session::AnalysisRequest request;
    request.files = invocation.files;
    request.defines = invocation.defines;
    request.includeDirectories = invocation.includeDirectories;
    if (!session.analyse(request)) return false;

    if (!invocation.waveformScope.empty() && !session.addWaveformScope(invocation.waveformScope))
        return false;

    static_cast<void>(diagnostics);
    return session.elaborate(invocation.topModule);
}

/// Serves an elaborated session to a graphical front end instead of running it here.
///
/// The client drives the run, so nothing happens until one attaches, and the kernel survives
/// its departure: section 2.2 requires a GUI to be able to detach from a long simulation on a
/// compute server without ending it.
ExitStatus serve(const Invocation& invocation, enzan::control::Session& session,
                 enzan::SourceManager& sources, enzan::DiagnosticEngine& diagnostics) {
    enzan::ipc::ServerOptions serverOptions;
    serverOptions.kernelVersion = enzan::versionLine();
    serverOptions.timeScale = enzan::runtime::TimeScale(invocation.timeExponent);
    serverOptions.deltaLimit = invocation.deltaLimit;
    serverOptions.waveformScope = invocation.waveformScope;

    enzan::ipc::KernelServer server(session, diagnostics, serverOptions);
    server.start();

    // The front end's command entry reaches the same interpreter a `do` file does, acting on
    // the same session. Two interpreters, or a protocol with a request per command, would be a
    // second control surface — which is the thing section 8.1 exists to prevent.
    enzan::control::Interpreter interpreter(diagnostics);
    interpreter.setTranscript(&server.transcript());
    enzan::control::registerCommands(interpreter, session, sources);

    /// Hands a client's typed line to the interpreter.
    class TclCommands final : public enzan::ipc::CommandEvaluator {
    public:
        explicit TclCommands(enzan::control::Interpreter& toUse) : interpreter(&toUse) {}

        bool evaluate(std::string_view script, std::string& result, std::string& error) override {
            const enzan::control::EvalResult outcome = interpreter->evaluate(script);
            if (outcome.status == enzan::control::EvalStatus::Error) {
                error = outcome.value;
                return false;
            }
            result = outcome.value;
            return true;
        }

    private:
        enzan::control::Interpreter* interpreter;
    };

    TclCommands commands(interpreter);
    if (interpreter.valid()) server.setCommandEvaluator(&commands);

    std::string error;
    if (!server.listen(invocation.listenPath, error)) {
        diagnostics.report(enzan::DiagCode::CannotOpenFile, enzan::SourceLocation(),
                           invocation.listenPath.string(), error);
        return ExitStatus::Failure;
    }

    // Printed rather than logged, because it is the one thing the user needs next and they are
    // looking at this terminal.
    std::cout << "-- listening on " << server.socketPath().string() << "\n"
              << "-- attach with: enzan-gui --attach " << server.socketPath().string() << '\n'
              << std::flush;

    const int status = server.serve();
    if (status != 0 || diagnostics.hasErrors()) return ExitStatus::Failure;
    return ExitStatus::Success;
}

/// Analyses, elaborates and runs. Returns the process exit status.
ExitStatus simulate(const Invocation& invocation, enzan::SourceManager& sources,
                    enzan::DiagnosticEngine& diagnostics) {
    enzan::control::Session session(sources, diagnostics);
    if (!prepare(invocation, session, diagnostics)) return ExitStatus::Failure;

    if (!invocation.listenPath.empty()) return serve(invocation, session, sources, diagnostics);

    // Opened after elaboration so that the values dumped at time zero are the ones the design
    // actually starts from rather than an empty arena.
    if (!invocation.vcdPath.empty() && !session.openVcd(invocation.vcdPath))
        return ExitStatus::Failure;
    if (!invocation.fstPath.empty() && !session.openFst(invocation.fstPath))
        return ExitStatus::Failure;

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

    if (status == enzan::runtime::RunStatus::Error || session.exitStatus() != 0
        || diagnostics.hasErrors()) {
        return ExitStatus::Failure;
    }
    return ExitStatus::Success;
}

#endif // ENZAN_WITH_SYSTEMVERILOG

/// Reads commands from the standard input until end of file or `quit`.
///
/// A line at a time rather than a readline-style editor: a line editor is a dependency and a
/// terminal-handling problem, and the shell a user drives this from already has one through
/// `rlwrap`. The prompt is printed only to a terminal, so that a script piped in produces a
/// transcript with nothing in it that was not asked for.
void readCommands(enzan::control::Interpreter& interpreter, enzan::SourceManager& sources) {
    const bool attached = ::isatty(STDIN_FILENO) != 0;
    std::string pending;

    while (!interpreter.stopRequested()) {
        if (attached) std::cout << (pending.empty() ? "enzan> " : "     > ") << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) break;

        pending += line;
        pending += '\n';

        // A `proc` or an `if` typed across several lines is evaluated once it is finished,
        // which is what the completeness test is for.
        if (!enzan::control::Interpreter::isComplete(pending)) continue;

        const enzan::BufferId buffer = sources.addBuffer("<stdin>", pending);
        const enzan::control::EvalResult result = interpreter.evaluateBuffer(buffer, pending);
        pending.clear();

        if (result.status == enzan::control::EvalStatus::Error) {
            interpreter.reportFailure(result);
            continue;
        }
        if (!result.value.empty()) std::cout << result.value << '\n';
    }

    // An unfinished command at end of input is a truncated script, not an empty one: saying so
    // beats exiting successfully having silently dropped it.
    if (!pending.empty() && !enzan::control::Interpreter::isComplete(pending))
        std::cerr << "enzan: the input ended in the middle of a command\n";
}

/// Runs a script, an interactive session, or both.
ExitStatus control(const Invocation& invocation, enzan::SourceManager& sources,
                   enzan::DiagnosticEngine& diagnostics) {
    enzan::control::Session session(sources, diagnostics);
    enzan::control::Interpreter interpreter(diagnostics);
    if (!interpreter.valid()) return ExitStatus::Failure;

    enzan::control::registerCommands(interpreter, session, sources);

    // Whatever else was on the command line is the script's, not the driver's. Collected from
    // the positional arguments wherever they appeared, so that `--do` does not have to come
    // first for them to be recognised.
    std::string argumentList;
    for (const std::filesystem::path& argument : invocation.files) {
        if (!argumentList.empty()) argumentList += ' ';
        argumentList += argument.string();
    }
    interpreter.setVariable("argv", argumentList);
    interpreter.setVariable("argc", std::to_string(invocation.files.size()));

    if (!invocation.scriptPath.empty()) {
        const enzan::control::EvalResult result =
            interpreter.evaluateFile(invocation.scriptPath, sources);
        if (result.status == enzan::control::EvalStatus::Error) {
            interpreter.reportFailure(result);
            // The prompt is still offered after a failed script, because that is the moment a
            // user most wants to look at what the design was left holding.
            if (!invocation.interactive) return ExitStatus::Failure;
        }
    }

    if (invocation.interactive && !interpreter.stopRequested()) readCommands(interpreter, sources);

    // Finished here as well as by `quit`, for the session that ended at end of input.
    session.closeWaveforms();

    if (interpreter.stopRequested() && interpreter.exitStatus() != 0) return ExitStatus::Failure;
    if (session.exitStatus() != 0) return ExitStatus::Failure;
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
    switch (parseCommandLine(arguments, invocation)) {
    case ParseOutcome::PrintedHelp: return static_cast<int>(ExitStatus::Success);
    case ParseOutcome::BadUsage: return static_cast<int>(ExitStatus::Usage);
    case ParseOutcome::Run: break;
    }

    const bool scripted = !invocation.scriptPath.empty() || invocation.interactive;

    // The control language is the whole interface when it is in play. Section 8.1 makes Tcl
    // *the* control language, and accepting the options above alongside it would leave two
    // ways to say the same thing that have to stay compatible with each other for ever.
    if (scripted) {
        const char* conflicting = nullptr;
        if (!invocation.topModule.empty())
            conflicting = "--top";
        else if (invocation.runUntil.has_value())
            conflicting = "--run";
        else if (!invocation.vcdPath.empty())
            conflicting = "--vcd";
        else if (!invocation.fstPath.empty())
            conflicting = "--fst";
        else if (!invocation.waveformScope.empty())
            conflicting = "--wave-scope";
        else if (!invocation.listenPath.empty())
            conflicting = "--listen";
        else if (!invocation.defines.empty())
            conflicting = "--define";
        else if (!invocation.includeDirectories.empty())
            conflicting = "--incdir";

        if (conflicting != nullptr) {
            std::cerr << "enzan: " << conflicting
                      << " cannot be combined with '--do' or '--interactive'; the script is "
                         "the whole interface, and `vlog`, `vsim`, `run`, `vcd file` and "
                         "`log` are what say these things inside one\n";
            return static_cast<int>(ExitStatus::Usage);
        }
    } else if (invocation.files.empty()) {
        std::cerr << "enzan: no source files were given\n"
                  << "Try 'enzan --help' for the available options.\n";
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
        if (!invocation.vcdPath.empty() || !invocation.fstPath.empty()) {
            std::cerr << "enzan: '--vcd' and '--fst' cannot be combined with '--listen'; a "
                         "served session records into the live waveform database, and writing "
                         "a file from an interactive session arrives with the control layer "
                         "at milestone M3\n";
            return static_cast<int>(ExitStatus::Usage);
        }
    }

    enzan::SourceManager sources;
    enzan::DiagnosticEngine diagnostics(sources);
    enzan::TextDiagnosticConsumer consumer(sources, std::cerr);
    diagnostics.addConsumer(consumer);

    if (scripted) return static_cast<int>(control(invocation, sources, diagnostics));

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
