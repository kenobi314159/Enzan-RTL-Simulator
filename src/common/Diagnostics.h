// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "common/SourceManager.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {

/// How seriously a diagnostic is to be taken.
///
/// Ordered from least to most severe so that comparisons work, with `Ignored`
/// first because a suppressed diagnostic is the absence of one. A single engine
/// serves the whole tool (DESIGN_GUIDE.md section 10.6), so these are the only
/// severities any component may use.
enum class DiagnosticSeverity : std::uint8_t {
    /// Suppressed: not formatted, not counted and not delivered to consumers.
    Ignored,
    /// Additional context attached to another diagnostic, or standalone
    /// information that is not a problem.
    Note,
    /// Something suspicious that does not prevent the tool from continuing.
    Warning,
    /// A problem that prevents the current operation from producing a usable
    /// result. Analysis continues so that the user sees more than one error.
    Error,
    /// A problem that makes continuing pointless or unsafe. The engine counts it
    /// and reports it; deciding to stop is the caller's, since only the caller
    /// knows what has to be torn down.
    Fatal,
};

/// Returns the lowercase word used in diagnostic output ("error", "warning").
std::string_view toString(DiagnosticSeverity severity) noexcept;

/// Every diagnostic Enzan can emit.
///
/// The enumerator's value is its stable public number: `DiagCode::CannotOpenFile`
/// is reported to users as `ENZ0001` and may be referenced in documentation and
/// suppressed by name. The catalogue, including the rules about numbering, is
/// DiagnosticCodes.def.
enum class DiagCode : std::uint16_t {
#define ENZAN_DIAG(name, number, severity, format) name = (number),
#include "common/DiagnosticCodes.def"
#undef ENZAN_DIAG
};

/// Returns the stable textual form of `code`, for example `"ENZ0001"`.
///
/// This string appears in user-facing output and documentation, so its shape —
/// `ENZ` followed by at least four zero-padded digits — must not change.
std::string toString(DiagCode code);

/// Returns the severity `code` has before any user remapping.
DiagnosticSeverity defaultSeverity(DiagCode code) noexcept;

/// Returns the std::format format string for `code`, as given in the catalogue.
std::string_view formatString(DiagCode code) noexcept;

/// Extra context attached to a diagnostic, such as where a conflicting
/// declaration was made.
struct DiagnosticNote {
    /// Where the note points. May be invalid, for a note with no position.
    SourceLocation location;
    std::string message;
};

/// One diagnostic, after severity remapping and message formatting.
///
/// This is a plain value: it is what a DiagnosticConsumer receives and what the
/// IPC layer will serialise to the GUI, so it deliberately holds finished text
/// rather than a code plus arguments to be formatted later.
struct Diagnostic {
    DiagCode code = DiagCode::InternalError;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;

    /// Primary position. May be invalid, for a diagnostic about the command line
    /// or about the design as a whole.
    SourceLocation location;

    /// The span to underline beneath the caret. Ignored unless valid and in the
    /// same buffer as `location`.
    SourceRange range;

    std::string message;
    std::vector<DiagnosticNote> notes;
};

/// Receives diagnostics from the engine.
///
/// Implementations render to a transcript, forward over IPC to the GUI, or
/// collect for a test. Consumers are called in the order they were registered,
/// which is part of the determinism guarantee: diagnostic ordering must be
/// reproducible across runs (DESIGN_GUIDE.md section 3.8).
class DiagnosticConsumer {
public:
    DiagnosticConsumer() = default;
    DiagnosticConsumer(const DiagnosticConsumer&) = delete;
    DiagnosticConsumer& operator=(const DiagnosticConsumer&) = delete;
    DiagnosticConsumer(DiagnosticConsumer&&) = delete;
    DiagnosticConsumer& operator=(DiagnosticConsumer&&) = delete;
    virtual ~DiagnosticConsumer() = default;

    /// Called once per delivered diagnostic. Must not report diagnostics of its
    /// own through the same engine; doing so would re-enter the engine while it
    /// is mid-delivery.
    virtual void consume(const Diagnostic& diagnostic) = 0;
};

class DiagnosticEngine;

/// Accumulates optional detail on a diagnostic, then reports it.
///
/// Returned by DiagnosticEngine::report so that notes and a highlight range can
/// be attached at the call site:
///
/// ```cpp
/// diagnostics.report(DiagCode::CannotOpenFile, location, path, reason)
///     .highlight(range)
///     .note(previous, "first referenced here");
/// ```
///
/// The diagnostic is delivered when the builder is destroyed, which is normally
/// the end of the full expression. Discarding the returned builder without
/// touching it still reports the diagnostic — that is the point, and it is why
/// the return value is not marked `[[nodiscard]]`.
class DiagnosticBuilder {
public:
    DiagnosticBuilder(const DiagnosticBuilder&) = delete;
    DiagnosticBuilder& operator=(const DiagnosticBuilder&) = delete;
    DiagnosticBuilder& operator=(DiagnosticBuilder&&) = delete;

    /// Transfers responsibility for reporting; the source becomes inert.
    DiagnosticBuilder(DiagnosticBuilder&& other) noexcept;

    /// Reports the diagnostic unless this builder is inert.
    ~DiagnosticBuilder();

    /// Attaches a note. Notes appear in the order they are added.
    DiagnosticBuilder& note(SourceLocation location, std::string message);

    /// Sets the span underlined beneath the caret.
    DiagnosticBuilder& highlight(SourceRange range);

private:
    friend class DiagnosticEngine;

    /// Constructs an inert builder, used when the code is suppressed.
    DiagnosticBuilder() noexcept = default;

    DiagnosticBuilder(DiagnosticEngine& owner, Diagnostic value);

    /// Null when inert: either the diagnostic was suppressed, or this builder
    /// has been moved from.
    DiagnosticEngine* engine = nullptr;
    Diagnostic diagnostic;
};

/// The single diagnostic engine that serves the whole tool.
///
/// Every component reports through one engine so that output is uniform
/// regardless of which frontend or phase produced it, including diagnostics
/// translated in from slang and NVC.
///
/// The engine formats the message, applies any severity remapping, maintains the
/// counts, enforces the error limit, and hands the result to each registered
/// consumer. It does not decide to stop: a caller that must abort on an error
/// checks `hasErrors()` at a point where it can unwind cleanly.
class DiagnosticEngine {
public:
    /// `sourceManager` must outlive the engine; it is used only by consumers that
    /// render source snippets, but is held here so that every consumer added to
    /// the engine agrees on which manager locations refer to.
    explicit DiagnosticEngine(const SourceManager& sourceManager) noexcept;

    DiagnosticEngine(const DiagnosticEngine&) = delete;
    DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;
    DiagnosticEngine(DiagnosticEngine&&) = delete;
    DiagnosticEngine& operator=(DiagnosticEngine&&) = delete;
    ~DiagnosticEngine() = default;

    /// Returns the source manager diagnostics from this engine refer to.
    [[nodiscard]] const SourceManager& sourceManager() const noexcept { return sources; }

    /// Registers `consumer`, which must outlive the engine or be removed first.
    /// Registering the same consumer twice delivers to it twice.
    void addConsumer(DiagnosticConsumer& consumer);

    /// Removes the first registration of `consumer`. Does nothing if it was
    /// never registered.
    void removeConsumer(DiagnosticConsumer& consumer);

    /// Formats and prepares a diagnostic of `code` at `location`.
    ///
    /// `args` are substituted into the code's format string. Pass a
    /// default-constructed SourceLocation for a diagnostic with no position.
    ///
    /// A suppressed code returns an inert builder without formatting anything,
    /// so suppression costs nothing beyond the severity lookup. A mismatch
    /// between the arguments and the format string cannot throw out of this
    /// function: it is a defect in the caller, and crashing while reporting an
    /// error would destroy the diagnostic that mattered, so it produces a
    /// self-describing placeholder message instead.
    template<typename... Args>
    DiagnosticBuilder report(DiagCode code, SourceLocation location, const Args&... args) {
        const DiagnosticSeverity effective = severity(code);
        if (effective == DiagnosticSeverity::Ignored || limitReached) return {};

        Diagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.severity = effective;
        diagnostic.location = location;
        diagnostic.message = formatMessage(code, std::make_format_args(args...));
        return {*this, std::move(diagnostic)};
    }

    /// Prepares a diagnostic of `code` whose message is already formatted: one another engine
    /// made, in a process of its own, and handed over. Severity overrides, suppression,
    /// counting and the error limit apply to it as to `report`.
    DiagnosticBuilder reportFormatted(DiagCode code, SourceLocation location, std::string message) {
        const DiagnosticSeverity effective = severity(code);
        if (effective == DiagnosticSeverity::Ignored || limitReached) return {};
        Diagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.severity = effective;
        diagnostic.location = location;
        diagnostic.message = std::move(message);
        return {*this, std::move(diagnostic)};
    }

    /// Overrides the severity of `code`.
    ///
    /// This is how `-Wno-ENZ0007` and "warnings as errors" are implemented. A
    /// code whose default severity is Fatal cannot be remapped: a Fatal
    /// diagnostic means continuing is unsafe, and letting a user downgrade one
    /// to a warning would turn a clean stop into wrong results, which is the
    /// failure mode this project weighs most heavily. Returns false when the
    /// request was refused for that reason.
    bool setSeverity(DiagCode code, DiagnosticSeverity severity);

    /// Suppresses `code` entirely. Equivalent to setting its severity to
    /// `Ignored`, and refused for Fatal codes for the same reason.
    bool suppress(DiagCode code);

    /// Clears any override for `code`, restoring its catalogue severity.
    void clearSeverity(DiagCode code);

    /// Returns the severity `code` will be reported at, after any override.
    [[nodiscard]] DiagnosticSeverity severity(DiagCode code) const;

    /// Sets the number of errors after which reporting stops, or zero for no
    /// limit.
    ///
    /// On reaching the limit the engine reports `TooManyErrors` and then ignores
    /// every subsequent diagnostic. A limit exists because a frontend that
    /// desynchronises can emit tens of thousands of errors, and a transcript
    /// that long hides the first one, which is the only one that mattered.
    void setErrorLimit(unsigned limit) noexcept;

    [[nodiscard]] unsigned errorLimit() const noexcept { return limit; }

    /// Returns how many diagnostics of exactly `severity` have been reported.
    [[nodiscard]] unsigned count(DiagnosticSeverity severity) const noexcept;

    /// Returns the number of Error plus Fatal diagnostics reported.
    [[nodiscard]] unsigned errorCount() const noexcept;

    [[nodiscard]] unsigned warningCount() const noexcept;

    /// True when any Error or Fatal diagnostic has been reported. This is the
    /// condition a phase checks before proceeding to the next one.
    [[nodiscard]] bool hasErrors() const noexcept { return errorCount() > 0; }

    /// True once the error limit has been reached and reporting has stopped.
    [[nodiscard]] bool errorLimitReached() const noexcept { return limitReached; }

    /// Resets the counts and the error-limit state, keeping consumers and
    /// severity overrides. Used between independent runs in one process, which
    /// is the normal case for the kernel driven from Tcl or the GUI.
    void reset() noexcept;

private:
    friend class DiagnosticBuilder;

    /// Formats a catalogue message, substituting a placeholder rather than
    /// propagating a std::format_error. Defined out of line so that the
    /// try/catch does not have to be repeated per instantiation of `report`.
    static std::string formatMessage(DiagCode code, std::format_args args);

    /// Counts, delivers and applies the error limit. Called by
    /// DiagnosticBuilder's destructor.
    void emit(const Diagnostic& diagnostic);

    /// Delivers to consumers without counting or limit checks, so that the
    /// TooManyErrors report cannot recurse into the limit logic.
    void deliver(const Diagnostic& diagnostic);

    const SourceManager& sources;

    /// Non-owning, in registration order. Ordered rather than hash-based because
    /// delivery order is observable in the transcript.
    std::vector<DiagnosticConsumer*> consumers;

    /// Ordered by code, so that iterating it — for example to print the active
    /// suppressions — is reproducible.
    std::map<DiagCode, DiagnosticSeverity> overrides;

    /// Indexed by the DiagnosticSeverity enumerator value.
    std::array<unsigned, 5> counts{};

    unsigned limit = 0;
    bool limitReached = false;
};

/// Reports a failed `SourceManager::readFile` as the diagnostic that fits its
/// cause.
///
/// Every phase that opens a file wants the same four-way mapping, and each one
/// writing its own drifts: a missing file, an unreadable one and one too large
/// to address are different problems with different codes, and collapsing them
/// into "could not open" throws away exactly the precision a diagnostic is
/// for. `path` is what the user asked for, so it is what is named.
void reportFileReadFailure(DiagnosticEngine& diagnostics, const std::filesystem::path& path,
                           const FileReadResult& result);

/// Renders diagnostics as text: position, severity, message, stable code, then
/// the source line with a caret.
///
/// ```text
/// counter.sv:12:16: error: cannot open file 'fifo.sv': No such file or directory [ENZ0001]
///    12 |     wire [7:0] result;
///       |                ^~~~~~
/// ```
///
/// Colour is not applied here. The transcript in the GUI colours diagnostics
/// from the theme configuration, and a terminal transcript that is redirected
/// to a file must not contain escape sequences.
class TextDiagnosticConsumer final : public DiagnosticConsumer {
public:
    /// Both `sourceManager` and `stream` must outlive the consumer.
    TextDiagnosticConsumer(const SourceManager& sourceManager, std::ostream& stream) noexcept;

    void consume(const Diagnostic& diagnostic) override;

    /// Turns the source line and caret off, leaving only the position line.
    /// Used by tests and by callers that want a compact transcript.
    void setShowSourceSnippets(bool show) noexcept { snippets = show; }

    [[nodiscard]] bool showSourceSnippets() const noexcept { return snippets; }

    /// Width a tab expands to when rendering a source line.
    ///
    /// A fixed width is unavoidable: the caret has to line up with the text
    /// above it, and the terminal's actual tab stops are unknowable. Eight
    /// matches what every other compiler assumes.
    static constexpr unsigned TabWidth = 8;

private:
    /// Writes the `file:line:column: severity: message [CODE]` line.
    void writeHeader(const Diagnostic& diagnostic);

    /// Writes the source line and the caret line beneath it, if `location` has a
    /// source line to show.
    void writeSnippet(SourceLocation location, SourceRange range);

    const SourceManager& sources;
    std::ostream& output;
    bool snippets = true;
};

} // namespace enzan
