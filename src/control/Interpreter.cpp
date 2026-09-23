// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Interpreter.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <tcl.h>

// Section 8.1 asks for Tcl 8.6 or later. Checked here rather than in CMake because CMake's
// FindTCL module reports no version at all, so the header is the only thing that knows.
static_assert(TCL_MAJOR_VERSION > 8 || (TCL_MAJOR_VERSION == 8 && TCL_MINOR_VERSION >= 6),
              "Enzan requires Tcl 8.6 or later (DESIGN_GUIDE.md section 8.1)");

namespace enzan::control {

namespace {

// Tcl 9 widened the counts in a command procedure's signature from `int` to `Tcl_Size`. The
// alias keeps one spelling of the procedures below working under either, so that a
// distribution moving to Tcl 9 is a rebuild rather than an edit.
#if TCL_MAJOR_VERSION >= 9
using TclSize = Tcl_Size;
#else
using TclSize = int;
#endif

/// Wraps text as a Tcl result object.
void setResult(Tcl_Interp* interp, std::string_view text) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj(text.data(), static_cast<TclSize>(text.size())));
}

/// The name the original `puts` is kept under once ours replaces it.
constexpr const char* OriginalPuts = "::enzan::tclPuts";

/// The script that moves the built-in `puts` aside.
///
/// Renaming rather than deleting: a script that writes to a file it opened itself is doing
/// something Enzan has no business breaking, so our command delegates back to this one for
/// every channel that is not the transcript.
constexpr const char* InstallPutsScript = R"(
namespace eval ::enzan {}
rename puts ::enzan::tclPuts
)";

} // namespace

/// One registered command: the handler, plus the interpreter it belongs to.
///
/// A separate object rather than a lambda converted to a function pointer, because the handler
/// is a `std::function` and Tcl's ClientData is a single pointer. The interpreter owns these
/// and destroys them with itself, which is what makes a handler's captures safe for exactly as
/// long as the command exists.
class Interpreter::Command {
public:
    Command(Interpreter& owner, CommandHandler function)
        : interpreter(&owner), handler(std::move(function)) {}

    /// Tcl's entry point. Marshals the arguments, runs the handler, publishes the result.
    static int invoke(ClientData data, Tcl_Interp* interp, TclSize objc, Tcl_Obj* const objv[]) {
        auto* command = static_cast<Command*>(data);

        // Stack-free and allocation-free would be nicer, but a command invocation is not the
        // simulation hot path: this runs once per typed line or per line of a script.
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(objc));
        for (TclSize index = 0; index < objc; ++index) {
            TclSize length = 0;
            const char* text = Tcl_GetStringFromObj(objv[index], &length);
            arguments.emplace_back(text, static_cast<std::size_t>(length));
        }

        const CommandResult result = command->handler(arguments);
        setResult(interp, result.value);

        switch (result.status) {
        case EvalStatus::Ok: return TCL_OK;
        case EvalStatus::Error: return TCL_ERROR;
        case EvalStatus::Stopped:
            // TCL_RETURN rather than TCL_ERROR, so that `quit` unwinds the script it was
            // called from without the interpreter treating it as a failure. Tcl turns a
            // TCL_RETURN that reaches the outermost evaluation back into TCL_OK, so the
            // intent is also recorded on the interpreter, which is what the result is
            // built from.
            command->interpreter->unwinding = true;
            return TCL_RETURN;
        }
        return TCL_OK;
    }

    /// Tcl's deletion callback. Does nothing: the interpreter owns the object, and freeing it
    /// here would double-free when a command is replaced and then the interpreter destroyed.
    static void discard(ClientData /*data*/) {}

private:
    Interpreter* interpreter;
    CommandHandler handler;
};

Interpreter::Interpreter(DiagnosticEngine& engine) : diagnostics(engine) {
    // Tcl asks to be told about the executable once per process before anything else. It is
    // what initialises the library's encodings, and skipping it leaves a handful of things
    // quietly not working.
    static std::once_flag initialised;
    std::call_once(initialised, [] { Tcl_FindExecutable(nullptr); });

    handle = Tcl_CreateInterp();
    if (handle == nullptr) {
        diagnostics.report(DiagCode::TclInterpreterFailed, SourceLocation());
        return;
    }

    // Tcl_Init finds and sources the script library — `auto_load`, `package require`, the
    // `::tcl` namespace. It fails on an installation whose library directory is missing, which
    // costs a user those facilities but not the interpreter: the whole command vocabulary of
    // section 8.1 is C++ and keeps working. A warning rather than a failure, because refusing
    // to start over a missing init.tcl would be a poor trade for anyone driving a kernel from
    // a container that ships only libtcl.
    if (Tcl_Init(handle) != TCL_OK) {
        diagnostics.report(DiagCode::TclLibraryUnavailable, SourceLocation(),
                           Tcl_GetStringResult(handle));
        Tcl_ResetResult(handle);
    }

    if (Tcl_Eval(handle, InstallPutsScript) != TCL_OK) {
        // Nothing user-visible has gone wrong yet, but script output would land on a terminal
        // rather than in the transcript, and a GUI user would see nothing at all.
        diagnostics.report(DiagCode::TranscriptNotRedirected, SourceLocation(),
                           Tcl_GetStringResult(handle));
        Tcl_ResetResult(handle);
    } else {
        registerCommand("puts", [this](CommandArguments arguments) -> CommandResult {
            return writeLine(arguments);
        });
    }
}

Interpreter::~Interpreter() {
    // The commands are deleted before the interpreter so that nothing Tcl runs during its own
    // teardown — a trace, a deletion callback — can reach a handler whose captures are gone.
    if (handle != nullptr) Tcl_DeleteInterp(handle);
    commands.clear();
}

void Interpreter::registerCommand(std::string_view name, CommandHandler handler) {
    if (handle == nullptr) return;

    auto command = std::make_unique<Command>(*this, std::move(handler));
    const std::string commandName(name);
    Tcl_CreateObjCommand(handle, commandName.c_str(), &Command::invoke, command.get(),
                         &Command::discard);
    commands.push_back(std::move(command));
}

bool Interpreter::hasCommand(std::string_view name) const {
    if (handle == nullptr) return false;

    const std::string commandName(name);
    Tcl_CmdInfo info;
    return Tcl_GetCommandInfo(handle, commandName.c_str(), &info) != 0;
}

CommandResult Interpreter::writeLine(CommandArguments arguments) {
    // `puts ?-nonewline? ?channelId? string`, with the channel defaulting to stdout.
    bool newline = true;
    std::size_t index = 1;
    if (index < arguments.size() && arguments[index] == "-nonewline") {
        newline = false;
        ++index;
    }

    std::string_view channel = "stdout";
    if (arguments.size() - index == 2) {
        channel = arguments[index];
        ++index;
    }

    if (arguments.size() - index != 1)
        return CommandResult::error("wrong # args: should be \"puts ?-nonewline? ?channelId? "
                                    "string\"");

    // stderr joins stdout rather than reaching the process's own error stream. The transcript
    // is where a user looks for what their script said, and splitting it across two
    // destinations means a GUI session silently loses half of it.
    if (channel != "stdout" && channel != "stderr") {
        // Not ours: hand it back to the interpreter's own `puts`, which knows about file
        // handles, sockets and encodings.
        std::string script = OriginalPuts;
        if (!newline) script += " -nonewline";
        for (std::size_t argument = index - 1; argument < arguments.size(); ++argument) {
            script += ' ';
            script += quoted(arguments[argument]);
        }

        if (Tcl_Eval(handle, script.c_str()) != TCL_OK)
            return CommandResult::error(Tcl_GetStringResult(handle));
        return CommandResult::ok();
    }

    transcriptStream() << arguments[index];
    if (newline) transcriptStream() << '\n';
    return CommandResult::ok();
}

std::string Interpreter::quoted(std::string_view text) {
    // Tcl's own quoting, so that a value containing a brace, a bracket or a newline survives
    // being spliced into a script. Written out rather than built with Tcl_Merge because the
    // result has to be a `std::string` either way and this is one argument.
    std::string result;
    result.reserve(text.size() + 2);
    result += '"';
    for (const char character : text) {
        if (character == '"' || character == '\\' || character == '$' || character == '[')
            result += '\\';
        result += character;
    }
    result += '"';
    return result;
}

EvalResult Interpreter::evaluate(std::string_view script) {
    if (handle == nullptr)
        return {.status = EvalStatus::Error, .value = "no Tcl interpreter is available"};

    const int code =
        Tcl_EvalEx(handle, script.data(), static_cast<TclSize>(script.size()), TCL_EVAL_DIRECT);
    return resultFrom(code, BufferId());
}

EvalResult Interpreter::evaluateBuffer(BufferId buffer, std::string_view text) {
    if (handle == nullptr)
        return {.status = EvalStatus::Error, .value = "no Tcl interpreter is available"};

    const int code =
        Tcl_EvalEx(handle, text.data(), static_cast<TclSize>(text.size()), TCL_EVAL_DIRECT);
    return resultFrom(code, buffer);
}

EvalResult Interpreter::evaluateFile(const std::filesystem::path& path, SourceManager& sources) {
    const FileReadResult read = sources.readFile(path);
    if (!read.ok()) {
        reportFileReadFailure(diagnostics, path, read);
        return {.status = EvalStatus::Error, .value = "could not read " + path.string()};
    }

    return evaluateBuffer(read.buffer, sources.bufferText(read.buffer));
}

EvalResult Interpreter::resultFrom(int code, BufferId buffer) {
    EvalResult result;
    result.value = Tcl_GetStringResult(handle);

    const bool stopped = std::exchange(unwinding, false);

    switch (code) {
    case TCL_OK: result.status = stopped ? EvalStatus::Stopped : EvalStatus::Ok; break;
    case TCL_RETURN: result.status = EvalStatus::Stopped; break;
    default: result.status = EvalStatus::Error; break;
    }

    if (result.status != EvalStatus::Error) return result;

    const char* info = Tcl_GetVar2(handle, "errorInfo", nullptr, TCL_GLOBAL_ONLY);
    if (info != nullptr) result.errorInfo = info;

    // Tcl counts the line within the script it was handed, which is the whole buffer, so the
    // number maps straight onto the source manager's index. `lineStart` gives the position of
    // the line rather than of the failing word: Tcl does not report a column, and pointing at
    // the start of the line is honest where guessing a column would not be.
    const int line = Tcl_GetErrorLine(handle);
    if (buffer.valid() && line > 0)
        result.location =
            diagnostics.sourceManager().lineStart(buffer, static_cast<unsigned>(line));

    return result;
}

void Interpreter::reportFailure(const EvalResult& result) {
    if (result.ok()) return;
    diagnostics.report(DiagCode::ScriptError, result.location, result.value);
}

std::ostream& Interpreter::transcriptStream() const {
    return transcript != nullptr ? *transcript : std::cout;
}

void Interpreter::setVariable(std::string_view name, std::string_view value) {
    if (handle == nullptr) return;

    const std::string variableName(name);
    const std::string text(value);
    Tcl_SetVar2(handle, variableName.c_str(), nullptr, text.c_str(), TCL_GLOBAL_ONLY);
}

std::string Interpreter::variable(std::string_view name) const {
    if (handle == nullptr) return {};

    const std::string variableName(name);
    const char* value = Tcl_GetVar2(handle, variableName.c_str(), nullptr, TCL_GLOBAL_ONLY);
    return value != nullptr ? std::string(value) : std::string();
}

void Interpreter::requestStop(int status) noexcept {
    stopping = true;
    requestedExitStatus = status;
}

} // namespace enzan::control
