// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Tcl's own type, forward declared so that <tcl.h> does not reach every file that drives a
// session. Its name is Tcl's to choose, not Enzan's.
// NOLINTNEXTLINE(readability-identifier-naming)
struct Tcl_Interp;

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::control {

/// What a command or a script evaluation produced.
enum class EvalStatus : std::uint8_t {
    /// The command succeeded; `value` is its result.
    Ok,
    /// The command failed; `value` is the message the user sees.
    Error,
    /// The script asked to stop — `quit`, or a top-level `return`. Not a failure.
    Stopped,
};

/// The outcome of a command.
///
/// A value rather than a pair of out-parameters so that a command body reads as a sequence of
/// early returns, which is what nearly every command is: check the arguments, do the thing,
/// hand back a string.
struct CommandResult {
    EvalStatus status = EvalStatus::Ok;

    /// The interpreter result on success, the message on failure.
    ///
    /// A command's result is what `set x [examine /tb/clk]` captures, so it is data and not
    /// transcript text. Anything a command wants *printed* it writes to the transcript stream.
    std::string value;

    [[nodiscard]] static CommandResult ok(std::string text = {}) {
        return {.status = EvalStatus::Ok, .value = std::move(text)};
    }

    [[nodiscard]] static CommandResult error(std::string message) {
        return {.status = EvalStatus::Error, .value = std::move(message)};
    }

    [[nodiscard]] static CommandResult stopped() { return {.status = EvalStatus::Stopped}; }
};

/// The arguments of one command invocation, with `args[0]` the name it was invoked by.
///
/// The views point into the interpreter's own argument objects and are valid only for the
/// duration of the call. A command that keeps a value past its return must copy it.
using CommandArguments = std::span<const std::string_view>;

/// A command implemented in C++.
using CommandHandler = std::function<CommandResult(CommandArguments)>;

/// The outcome of evaluating a script.
struct EvalResult {
    EvalStatus status = EvalStatus::Ok;

    /// The script's result, or the error message.
    std::string value;

    /// Tcl's `::errorInfo` stack trace, empty unless the evaluation failed.
    ///
    /// Kept separate from `value` because the two are read at different times: the message is
    /// what the user needs, the trace is what they need once the message was not enough.
    std::string errorInfo;

    /// Where the failure was, when the script came from a buffer the SourceManager holds.
    ///
    /// Invalid for a script evaluated from a string with no buffer behind it, and for a
    /// successful evaluation.
    SourceLocation location;

    [[nodiscard]] bool ok() const noexcept { return status != EvalStatus::Error; }
};

/// An embedded Tcl interpreter with Enzan's commands installed.
///
/// Tcl rather than a language of Enzan's own because section 8.1 fixes it: users arrive from
/// Questa, Vivado and Xcelium already knowing it, their existing scripts are written in it, and
/// it adds one small, ubiquitous dependency rather than a parser to maintain.
///
/// **Commands are C++ callables, not Tcl procs.** Registering a handler installs a Tcl command
/// that marshals its arguments into string views and its result back. The handler decides
/// whether it succeeded; anything it reports through the diagnostic engine reaches the
/// transcript by the ordinary route, so a command failure and a frontend error look the same to
/// the user, which is the point of one engine serving the whole tool (section 10.6).
///
/// **Output.** `puts` is replaced by one that writes to this interpreter's transcript stream,
/// so that a GUI attached to a kernel sees script output in the Transcript window rather than
/// on a terminal nobody is looking at. The original is kept as `::tcl::puts` and is delegated
/// to whenever a channel other than `stdout` or `stderr` is named, so writing to a file from a
/// script still works.
///
/// **Lifetime.** Handlers are owned by the interpreter and are destroyed with it. A handler
/// capturing a reference to something outside must therefore outlive the interpreter, which in
/// practice means the session owns the interpreter and not the other way round.
class Interpreter {
public:
    /// `engine` must outlive the interpreter.
    explicit Interpreter(DiagnosticEngine& engine);

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) = delete;
    Interpreter& operator=(Interpreter&&) = delete;
    ~Interpreter();

    /// True when the interpreter came up and can evaluate.
    ///
    /// False only if Tcl itself refused to create an interpreter, which has already been
    /// reported as a diagnostic by the time the constructor returns.
    [[nodiscard]] bool valid() const noexcept { return handle != nullptr; }

    /// Installs `handler` as the command `name`, replacing any command of that name.
    ///
    /// Replacing is deliberate and is how the vocabulary is layered: the session installs
    /// `run`, a test replaces it with one that records what it was asked, and a user's script
    /// can rename either.
    void registerCommand(std::string_view name, CommandHandler handler);

    /// True when a command of `name` exists, whether from C++ or from a script.
    [[nodiscard]] bool hasCommand(std::string_view name) const;

    /// Evaluates `script`, which is not associated with any source buffer.
    ///
    /// Used for the command line's `-do`, for a line typed at the prompt, and in tests. A
    /// failure carries no location, because there is no file to point into.
    EvalResult evaluate(std::string_view script);

    /// Reads `path` through the source manager and evaluates it.
    ///
    /// Read through the manager rather than by Tcl's own `source` so that an error inside the
    /// script gets a location with a file, a line and a caret like every other diagnostic
    /// (section 10.6) — `source` would report a line number in a message and nothing more.
    EvalResult evaluateFile(const std::filesystem::path& path, SourceManager& sources);

    /// Evaluates `text` as though it had come from `buffer`, so failures point into it.
    EvalResult evaluateBuffer(BufferId buffer, std::string_view text);

    /// True when `script` is a complete command rather than one waiting for a closing brace.
    ///
    /// What an interactive prompt asks between lines, so that a `proc` or an `if` typed across
    /// several lines is evaluated once it is finished rather than reported as a syntax error
    /// on the first one.
    [[nodiscard]] static bool isComplete(std::string_view script);

    /// Reports `result` as a control-layer diagnostic, unless it succeeded.
    ///
    /// The single place a failed evaluation becomes user-visible output, so that a script run
    /// from the command line, from `do`, and from the GUI all report identically.
    void reportFailure(const EvalResult& result);

    /// Where command output goes. Null, the default, means the standard output.
    void setTranscript(std::ostream* stream) noexcept { transcript = stream; }

    [[nodiscard]] std::ostream& transcriptStream() const;

    /// Sets a Tcl variable, which is how a command publishes state a script can read.
    void setVariable(std::string_view name, std::string_view value);

    /// Returns the value of a Tcl variable, or an empty string when it is unset.
    [[nodiscard]] std::string variable(std::string_view name) const;

    /// True once a command has called `requestStop`.
    [[nodiscard]] bool stopRequested() const noexcept { return stopping; }

    /// The status `quit` asked the process to exit with.
    [[nodiscard]] int exitStatus() const noexcept { return requestedExitStatus; }

    /// Asks the driving loop to stop; called by `quit`.
    ///
    /// A flag rather than Tcl's `exit`, which calls `Tcl_Exit` and takes the process with it.
    /// The kernel has a waveform file to close and a socket to unlink, and a control language
    /// that can terminate the process from under them would make `quit` lossy.
    void requestStop(int status) noexcept;

    /// The underlying interpreter, for the few places that need Tcl's own API.
    [[nodiscard]] Tcl_Interp* raw() const noexcept { return handle; }

private:
    class Command;

    /// Turns a Tcl return code plus the interpreter state into an `EvalResult`.
    EvalResult resultFrom(int code, BufferId buffer);

    /// The replacement `puts`: writes to the transcript, or delegates for another channel.
    CommandResult writeLine(CommandArguments arguments);

    /// Quotes `text` so that it survives being spliced into a Tcl script.
    [[nodiscard]] static std::string quoted(std::string_view text);

    DiagnosticEngine& diagnostics;
    Tcl_Interp* handle = nullptr;
    std::ostream* transcript = nullptr;

    /// Owned so that a handler's captures live exactly as long as the command does.
    std::vector<std::unique_ptr<Command>> commands;

    /// Set by a handler that returned `Stopped`, cleared when the result is built.
    ///
    /// Recorded rather than read back off Tcl's return code, because Tcl converts a
    /// `TCL_RETURN` reaching the outermost evaluation into `TCL_OK` — correct for `return`,
    /// which means "leave this proc", and indistinguishable from success for `quit`, which
    /// means "leave the session".
    bool unwinding = false;

    bool stopping = false;
    int requestedExitStatus = 0;
};

} // namespace enzan::control
