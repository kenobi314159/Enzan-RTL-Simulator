// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Commands.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "control/FileList.h"
#include "control/Interpreter.h"
#include "control/Session.h"
#include "eir/BitVector.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::control {

namespace {

/// A command's arguments after `args[0]`, so that a body reads the way the user typed it.
std::string_view argumentAt(CommandArguments arguments, std::size_t index) {
    return index < arguments.size() ? arguments[index] : std::string_view();
}

CommandResult usage(std::string_view text) {
    return CommandResult::error("wrong # args: should be \"" + std::string(text) + '"');
}

/// Parses a radix name, accepting both the `-radix hex` and the bare `hex` spellings.
std::optional<eir::Radix> parseRadix(std::string_view name) {
    if (name == "binary" || name == "bin" || name == "b") return eir::Radix::Binary;
    if (name == "octal" || name == "oct" || name == "o") return eir::Radix::Octal;
    if (name == "decimal" || name == "dec" || name == "d") return eir::Radix::Decimal;
    if (name == "hexadecimal" || name == "hex" || name == "h") return eir::Radix::Hexadecimal;
    return std::nullopt;
}

std::string_view radixName(eir::Radix radix) {
    switch (radix) {
    case eir::Radix::Binary: return "binary";
    case eir::Radix::Octal: return "octal";
    case eir::Radix::Decimal: return "decimal";
    case eir::Radix::Hexadecimal: return "hexadecimal";
    }
    return "hexadecimal";
}

/// Parses an unsigned decimal, rejecting anything that is not entirely digits.
std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
    return value;
}

/// Parses a time unit name into its power-of-ten exponent.
std::optional<runtime::TimeExponent> parseTimeUnit(std::string_view text) {
    if (text == "s") return 0;
    if (text == "ms") return -3;
    if (text == "us") return -6;
    if (text == "ns") return -9;
    if (text == "ps") return -12;
    if (text == "fs") return -15;
    return std::nullopt;
}

/// Converts `amount` of `unit` into the session's own time units.
///
/// The conversion is integer throughout, which is what section 3.1 requires: time is never in
/// floating point anywhere in the kernel, and `run 1.5ns` on a picosecond design would be the
/// one place a rounding decision crept in. A unit coarser than the precision multiplies; a unit
/// finer than it is refused rather than truncated, because silently turning `run 1fs` into
/// `run 0` would make a script appear to run and do nothing.
std::optional<runtime::SimTime> convertTime(std::uint64_t amount, runtime::TimeExponent unit,
                                            runtime::TimeScale scale) {
    const int difference = unit - scale.precisionExponent();
    if (difference < 0) return std::nullopt;

    runtime::SimTime value = amount;
    for (int step = 0; step < difference; ++step) {
        if (value > runtime::NoTime / 10) return std::nullopt;
        value *= 10;
    }
    return value;
}

/// Splits `+incdir+a+b` into its parts, dropping the empty ones.
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

/// Everything the commands share: the session they act on and where they report.
///
/// Held by value in each handler's capture would mean one copy per command, so the handlers
/// capture a pointer to the single instance the interpreter's registration owns.
class Vocabulary {
public:
    Vocabulary(Interpreter& interpreterToUse, Session& sessionToDrive, SourceManager& sourceManager)
        : interpreter(&interpreterToUse), session(&sessionToDrive), sources(&sourceManager) {}

    // --- helpers -----------------------------------------------------------

    [[nodiscard]] std::ostream& out() const { return session->transcriptStream(); }

    /// Resolves a signal path, reporting through the result when there is no such signal.
    ///
    /// Reported as a command error rather than as a diagnostic: a script that examines a
    /// signal that may not exist can catch a Tcl error, and a diagnostic would also raise the
    /// error count and make an otherwise successful run look failed.
    [[nodiscard]] std::uint32_t signalOr(std::string_view path, CommandResult& failure) const {
        const std::uint32_t index = session->findSignal(path);
        if (index == Session::NoSignal)
            failure = CommandResult::error("no signal named '" + std::string(path) + '\'');
        return index;
    }

    [[nodiscard]] bool requireSimulation(std::string_view command, CommandResult& failure) const {
        if (session->isElaborated()) return true;
        failure = CommandResult::error('`' + std::string(command)
                                       + "` needs a simulation; run `vsim <top>` first");
        return false;
    }

    // --- commands ----------------------------------------------------------

    CommandResult vlog(CommandArguments arguments);
    CommandResult vcom(CommandArguments arguments);
    CommandResult vsim(CommandArguments arguments);
    CommandResult run(CommandArguments arguments);
    CommandResult restart(CommandArguments arguments);
    CommandResult quit(CommandArguments arguments);
    CommandResult doFile(CommandArguments arguments);
    CommandResult examine(CommandArguments arguments);
    CommandResult force(CommandArguments arguments);
    CommandResult noforce(CommandArguments arguments);
    CommandResult deposit(CommandArguments arguments);
    CommandResult find(CommandArguments arguments);
    CommandResult radix(CommandArguments arguments);
    CommandResult log(CommandArguments arguments);
    CommandResult add(CommandArguments arguments);
    CommandResult vcd(CommandArguments arguments);
    CommandResult fst(CommandArguments arguments);

    /// Reports where the run stopped, the way a transcript line does.
    void reportRunOutcome(runtime::RunStatus status) const;

private:
    Interpreter* interpreter;
    Session* session;
    SourceManager* sources;

    /// The radix `examine` uses when it is not given one, set by `radix`.
    ///
    /// Absent means "by the name the type gives it where there is one", which is what the wave
    /// window shows and is not a radix; that is why this is an optional rather than defaulting
    /// to hexadecimal (section 10.3).
    std::optional<eir::Radix> defaultRadix;
};

// ---------------------------------------------------------------------------
// vlog
// ---------------------------------------------------------------------------

CommandResult Vocabulary::vlog(CommandArguments arguments) {
    Session::AnalysisRequest request;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == "-f" || argument == "-F") {
            if (index + 1 >= arguments.size())
                return CommandResult::error("'" + std::string(argument) + "' needs a file");

            FileList list;
            if (!readFileList(std::string(arguments[++index]), *sources,
                              // The engine is reached through the session so that a file-list
                              // failure is reported exactly as an analysis failure is.
                              session->diagnosticEngine(), list))
                return CommandResult::error("the file list could not be read");

            for (const std::filesystem::path& file : list.files) request.files.push_back(file);
            for (const std::string& define : list.defines) request.defines.push_back(define);
            for (const std::filesystem::path& directory : list.includeDirectories)
                request.includeDirectories.push_back(directory);
            continue;
        }

        if (argument.starts_with("+incdir+")) {
            for (const std::string_view part : splitPlusList(argument.substr(8)))
                request.includeDirectories.emplace_back(part);
            continue;
        }

        if (argument.starts_with("+define+")) {
            for (const std::string_view part : splitPlusList(argument.substr(8)))
                request.defines.emplace_back(part);
            continue;
        }

        // `-sv` asks for the only language this command reads, so it is accepted and dropped
        // rather than refused, exactly as the file-list reader treats it.
        if (argument == "-sv") continue;

        if (argument.starts_with('-') || argument.starts_with('+'))
            return CommandResult::error("vlog does not understand '" + std::string(argument)
                                        + '\'');

        request.files.emplace_back(argument);
    }

    if (!session->analyse(request)) return CommandResult::error("analysis failed");
    return CommandResult::ok();
}

// Static in the sense clang-tidy means — it reads no member — and a member in the sense that
// matters: `install` binds every command as a pointer to member, and one exception would mean
// a second binding path for the one command that does nothing.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
CommandResult Vocabulary::vcom(CommandArguments /*arguments*/) {
    return CommandResult::error(
        "VHDL analysis arrives with milestone M6; `vcom` is refused rather than ignored so "
        "that a mixed-language script fails here instead of simulating half a design");
}

// ---------------------------------------------------------------------------
// vsim, run, restart
// ---------------------------------------------------------------------------

CommandResult Vocabulary::vsim(CommandArguments arguments) {
    std::string top;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == "-t" || argument == "-timescale") {
            if (index + 1 >= arguments.size())
                return CommandResult::error("'-t' needs a time unit");

            const std::string_view unit = arguments[++index];
            const std::optional<runtime::TimeExponent> exponent = parseTimeUnit(unit);
            if (!exponent.has_value())
                return CommandResult::error("'" + std::string(unit)
                                            + "' is not a time unit; expected s, ms, us, ns, "
                                              "ps or fs");
            session->setTimeScale(runtime::TimeScale(*exponent));
            continue;
        }

        if (argument == "-deltalimit") {
            if (index + 1 >= arguments.size())
                return CommandResult::error("'-deltalimit' needs a count");

            const std::optional<std::uint64_t> limit = parseUnsigned(arguments[++index]);
            if (!limit.has_value() || *limit == 0 || *limit > 0xFFFF'FFFFU)
                return CommandResult::error("'" + std::string(arguments[index])
                                            + "' is not a usable delta limit");
            session->setDeltaLimit(static_cast<std::uint32_t>(*limit));
            continue;
        }

        if (argument.starts_with('-'))
            return CommandResult::error("vsim does not understand '" + std::string(argument)
                                        + '\'');

        if (!top.empty())
            return CommandResult::error("vsim takes one top level, and was given '" + top
                                        + "' and '" + std::string(argument) + '\'');
        top = argument;
    }

    if (!session->elaborate(top)) return CommandResult::error("elaboration failed");

    out() << "# Loading " << (top.empty() ? std::string("the design") : top) << '\n';
    return CommandResult::ok();
}

void Vocabulary::reportRunOutcome(runtime::RunStatus status) const {
    const std::string now = session->timeScale().format(session->time());

    switch (status) {
    case runtime::RunStatus::TimeLimitReached: out() << "# " << now << '\n'; return;
    case runtime::RunStatus::Quiescent: out() << "# " << now << ": nothing left to do\n"; return;
    case runtime::RunStatus::Finished: out() << "# " << now << ": $finish\n"; return;
    case runtime::RunStatus::Error: out() << "# " << now << ": the run was abandoned\n"; return;
    case runtime::RunStatus::Interrupted: out() << "# " << now << ": interrupted\n"; return;
    }
}

CommandResult Vocabulary::run(CommandArguments arguments) {
    CommandResult failure;
    if (!requireSimulation("run", failure)) return failure;

    std::optional<runtime::SimTime> until;

    // `run` with nothing runs to the end, which is what `run -all` says explicitly. Questa's
    // bare `run` advances by the default run length instead; Enzan has no such setting, and
    // inventing one so that the two commands could differ would be a setting nobody asked for.
    if (arguments.size() > 1) {
        const std::string_view first = arguments[1];

        if (first == "-all" || first == "-continue") {
            if (arguments.size() != 2) return usage("run -all");
        } else {
            // `run 100 ns` and `run 100ns` are both written in practice.
            std::string_view amountText = first;
            std::string_view unitText = argumentAt(arguments, 2);
            if (unitText.empty()) {
                const std::size_t split = amountText.find_first_not_of("0123456789");
                if (split != std::string_view::npos) {
                    unitText = amountText.substr(split);
                    amountText = amountText.substr(0, split);
                }
            } else if (arguments.size() != 3) {
                return usage("run ?time ?unit??");
            }

            const std::optional<std::uint64_t> amount = parseUnsigned(amountText);
            if (!amount.has_value())
                return CommandResult::error("'" + std::string(first) + "' is not a time");

            runtime::SimTime duration = *amount;
            if (!unitText.empty()) {
                const std::optional<runtime::TimeExponent> exponent = parseTimeUnit(unitText);
                if (!exponent.has_value())
                    return CommandResult::error("'" + std::string(unitText)
                                                + "' is not a time unit; expected s, ms, us, "
                                                  "ns, ps or fs");

                const std::optional<runtime::SimTime> converted =
                    convertTime(*amount, *exponent, session->timeScale());
                if (!converted.has_value())
                    return CommandResult::error(
                        "a delay of " + std::string(first)
                        + " cannot be expressed in this simulation's time unit of "
                        + session->timeScale().unitName());
                duration = *converted;
            }

            if (duration > runtime::NoTime - session->time())
                return CommandResult::error("that run would advance time past the 64-bit range");
            until = session->time() + duration;
        }
    }

    reportRunOutcome(session->run(until));
    return CommandResult::ok();
}

CommandResult Vocabulary::restart(CommandArguments arguments) {
    if (arguments.size() != 1) return usage("restart");

    CommandResult failure;
    if (!requireSimulation("restart", failure)) return failure;

    if (!session->restart()) return CommandResult::error("the design could not be restarted");
    out() << "# restarted\n";
    return CommandResult::ok();
}

// ---------------------------------------------------------------------------
// quit, do
// ---------------------------------------------------------------------------

CommandResult Vocabulary::quit(CommandArguments arguments) {
    int status = 0;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == "-code") {
            if (index + 1 >= arguments.size()) return CommandResult::error("'-code' needs a value");
            const std::optional<std::uint64_t> value = parseUnsigned(arguments[++index]);
            if (!value.has_value() || *value > 255)
                return CommandResult::error("'" + std::string(arguments[index])
                                            + "' is not an exit status");
            status = static_cast<int>(*value);
            continue;
        }

        // Accepted and ignored: `-f` and `-sim` are Questa's "do not ask" and "end the
        // simulation but stay in the tool", and neither has anything to decline here.
        if (argument == "-f" || argument == "-force" || argument == "-sim") continue;

        return CommandResult::error("quit does not understand '" + std::string(argument) + '\'');
    }

    // The files are finished here rather than at process exit, so that a `quit` in a script is
    // the point at which a waveform becomes readable.
    session->closeWaveforms();
    interpreter->requestStop(status);
    return CommandResult::stopped();
}

CommandResult Vocabulary::doFile(CommandArguments arguments) {
    if (arguments.size() < 2) return usage("do file ?arg ...?");

    // The arguments a `do` script sees, exactly as Questa hands them over.
    std::string argumentList;
    for (std::size_t index = 2; index < arguments.size(); ++index) {
        if (!argumentList.empty()) argumentList += ' ';
        argumentList += arguments[index];
    }
    interpreter->setVariable("argv", argumentList);
    interpreter->setVariable("argc", std::to_string(arguments.size() - 2));

    const EvalResult result = interpreter->evaluateFile(std::string(arguments[1]), *sources);
    if (result.status == EvalStatus::Error) {
        interpreter->reportFailure(result);
        return CommandResult::error("the script failed");
    }

    // A `quit` inside the script stops the outer one too, which is what a user who put one
    // there meant; a plain `return` at the end of a do file does not.
    if (interpreter->stopRequested()) return CommandResult::stopped();
    return CommandResult::ok(result.value);
}

// ---------------------------------------------------------------------------
// examine, force, deposit
// ---------------------------------------------------------------------------

CommandResult Vocabulary::examine(CommandArguments arguments) {
    CommandResult failure;
    if (!requireSimulation("examine", failure)) return failure;

    std::optional<eir::Radix> radix = defaultRadix;
    std::vector<std::string_view> paths;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        if (argument == "-radix") {
            if (index + 1 >= arguments.size())
                return CommandResult::error("'-radix' needs a radix");
            radix = parseRadix(arguments[++index]);
            if (!radix.has_value())
                return CommandResult::error("'" + std::string(arguments[index])
                                            + "' is not a radix");
            continue;
        }

        if (argument == "-name") {
            // The opposite of `-radix`: show the name an enumeration gives the value.
            radix.reset();
            continue;
        }

        if (argument.starts_with('-'))
            return CommandResult::error("examine does not understand '" + std::string(argument)
                                        + '\'');

        paths.push_back(argument);
    }

    if (paths.empty()) return usage("examine ?-radix radix? path ?path ...?");

    // A Tcl list, so that `examine /tb/a /tb/b` is indexable and a single path is just its
    // value — which is what makes `if {[examine /tb/done] == 1}` read naturally.
    std::string result;
    for (const std::string_view path : paths) {
        const std::uint32_t signalIndex = signalOr(path, failure);
        if (signalIndex == Session::NoSignal) return failure;

        if (!result.empty()) result += ' ';
        result += session->examine(signalIndex, radix);
    }
    return CommandResult::ok(result);
}

CommandResult Vocabulary::force(CommandArguments arguments) {
    if (arguments.size() != 3) return usage("force path value");

    CommandResult failure;
    if (!requireSimulation("force", failure)) return failure;

    const std::uint32_t signalIndex = signalOr(arguments[1], failure);
    if (signalIndex == Session::NoSignal) return failure;

    std::string error;
    if (!session->force(signalIndex, arguments[2], error)) return CommandResult::error(error);
    return CommandResult::ok();
}

CommandResult Vocabulary::noforce(CommandArguments arguments) {
    if (arguments.size() < 2) return usage("noforce path ?path ...?");

    CommandResult failure;
    if (!requireSimulation("noforce", failure)) return failure;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::uint32_t signalIndex = signalOr(arguments[index], failure);
        if (signalIndex == Session::NoSignal) return failure;

        // Releasing something that was not forced is not an error: a script that clears a set
        // of signals it may or may not have forced should not have to remember which.
        session->release(signalIndex);
    }
    return CommandResult::ok();
}

CommandResult Vocabulary::deposit(CommandArguments arguments) {
    if (arguments.size() != 3) return usage("deposit path value");

    CommandResult failure;
    if (!requireSimulation("deposit", failure)) return failure;

    const std::uint32_t signalIndex = signalOr(arguments[1], failure);
    if (signalIndex == Session::NoSignal) return failure;

    std::string error;
    if (!session->deposit(signalIndex, arguments[2], error)) return CommandResult::error(error);
    return CommandResult::ok();
}

// ---------------------------------------------------------------------------
// find, radix
// ---------------------------------------------------------------------------

CommandResult Vocabulary::find(CommandArguments arguments) {
    if (arguments.size() != 3) return usage("find signals|instances pattern");

    CommandResult failure;
    if (!requireSimulation("find", failure)) return failure;

    const std::string_view kind = arguments[1];
    std::vector<std::string> found;
    if (kind == "signals" || kind == "nets") {
        found = session->findSignals(arguments[2]);
    } else if (kind == "instances") {
        found = session->findInstances(arguments[2]);
    } else {
        return CommandResult::error("find takes `signals` or `instances`, not '" + std::string(kind)
                                    + '\'');
    }

    std::string result;
    for (const std::string& path : found) {
        if (!result.empty()) result += ' ';
        result += path;
    }
    return CommandResult::ok(result);
}

CommandResult Vocabulary::radix(CommandArguments arguments) {
    if (arguments.size() == 1)
        return CommandResult::ok(defaultRadix.has_value() ? std::string(radixName(*defaultRadix))
                                                          : std::string("symbolic"));

    if (arguments.size() != 2) return usage("radix ?radix?");

    std::string_view name = arguments[1];
    if (name.starts_with('-')) name.remove_prefix(1);

    // `symbolic` is Questa's name for showing an enumeration by its member name, which is what
    // Enzan does when no radix is chosen.
    if (name == "symbolic") {
        defaultRadix.reset();
        return CommandResult::ok("symbolic");
    }

    const std::optional<eir::Radix> parsed = parseRadix(name);
    if (!parsed.has_value())
        return CommandResult::error("'" + std::string(arguments[1]) + "' is not a radix");

    defaultRadix = parsed;
    return CommandResult::ok(std::string(radixName(*parsed)));
}

// ---------------------------------------------------------------------------
// log, add wave, vcd file, fst file
// ---------------------------------------------------------------------------

CommandResult Vocabulary::log(CommandArguments arguments) {
    std::vector<std::string_view> paths;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];

        // `-r` asks for the subtree, which is what naming a scope already means here: a scope
        // selects itself and everything under it. Accepted for the scripts that carry it.
        if (argument == "-r" || argument == "-recursive") continue;

        if (argument.starts_with('-'))
            return CommandResult::error("log does not understand '" + std::string(argument) + '\'');

        paths.push_back(argument);
    }

    if (paths.empty()) return usage("log ?-r? path ?path ...?");

    for (std::string_view path : paths) {
        // `/*` is how a script says "everything", and an empty selection is how the session
        // says it, so the two are the same request.
        if (path == "/*" || path == "*") path = {};
        if (!session->addWaveformScope(path))
            return CommandResult::error("the recording scope could not be changed");
    }
    return CommandResult::ok();
}

CommandResult Vocabulary::add(CommandArguments arguments) {
    if (arguments.size() < 2) return usage("add wave path ?path ...?");
    if (arguments[1] != "wave")
        return CommandResult::error("add takes `wave`, not '" + std::string(arguments[1]) + '\'');

    // `add wave` and `log` name the same thing in a headless kernel: which signals are
    // recorded. Which of them a *window* then shows is the GUI's business, and the GUI does
    // not reach it through Tcl.
    std::vector<std::string_view> forwarded(arguments.begin() + 1, arguments.end());
    return log(forwarded);
}

CommandResult Vocabulary::vcd(CommandArguments arguments) {
    if (arguments.size() != 3 || arguments[1] != "file") return usage("vcd file path");

    CommandResult failure;
    if (!requireSimulation("vcd file", failure)) return failure;

    if (!session->openVcd(std::string(arguments[2])))
        return CommandResult::error("the VCD file could not be opened");

    out() << "# recording a VCD to " << arguments[2] << '\n';
    return CommandResult::ok();
}

CommandResult Vocabulary::fst(CommandArguments arguments) {
    if (arguments.size() != 3 || arguments[1] != "file") return usage("fst file path");

    CommandResult failure;
    if (!requireSimulation("fst file", failure)) return failure;

    if (!session->openFst(std::string(arguments[2])))
        return CommandResult::error("the FST file could not be opened");

    out() << "# recording an FST to " << arguments[2] << '\n';
    return CommandResult::ok();
}

/// Registers every command, each handler holding the vocabulary alive.
///
/// Shared ownership rather than a single owner the caller has to keep, because the handlers
/// are what read the shared state — the default radix — and they live in the interpreter. A
/// caller that had to remember to outlive the interpreter would be a use-after-free waiting
/// for whoever wrote the second front end.
void install(const std::shared_ptr<Vocabulary>& vocabulary, Interpreter& interpreter) {
    const auto bind = [&vocabulary,
                       &interpreter](std::string_view name,
                                     CommandResult (Vocabulary::*method)(CommandArguments)) {
        interpreter.registerCommand(name, [vocabulary, method](CommandArguments arguments) {
            return ((*vocabulary).*method)(arguments);
        });
    };

    bind("vlog", &Vocabulary::vlog);
    bind("vcom", &Vocabulary::vcom);
    bind("vsim", &Vocabulary::vsim);
    bind("run", &Vocabulary::run);
    bind("restart", &Vocabulary::restart);
    bind("quit", &Vocabulary::quit);
    bind("exit", &Vocabulary::quit);
    bind("do", &Vocabulary::doFile);
    bind("examine", &Vocabulary::examine);
    bind("force", &Vocabulary::force);
    bind("noforce", &Vocabulary::noforce);
    bind("deposit", &Vocabulary::deposit);
    bind("find", &Vocabulary::find);
    bind("radix", &Vocabulary::radix);
    bind("log", &Vocabulary::log);
    bind("add", &Vocabulary::add);
    bind("vcd", &Vocabulary::vcd);
    bind("fst", &Vocabulary::fst);
}

} // namespace

void registerCommands(Interpreter& interpreter, Session& session, SourceManager& sources) {
    install(std::make_shared<Vocabulary>(interpreter, session, sources), interpreter);
}

} // namespace enzan::control
