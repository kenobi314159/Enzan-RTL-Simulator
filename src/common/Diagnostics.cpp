// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Diagnostics.h"

#include "common/Assert.h"

#include <algorithm>
#include <ostream>
#include <ranges>
#include <utility>

namespace enzan {

std::string_view toString(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::Ignored: return "ignored";
    case DiagnosticSeverity::Note: return "note";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Error: return "error";
    case DiagnosticSeverity::Fatal: return "fatal error";
    }
    return "error";
}

std::string toString(DiagCode code) {
    // Four digits are the documented shape; a hypothetical five-digit code would
    // simply be wider rather than truncated.
    return std::format("ENZ{:04}", static_cast<unsigned>(code));
}

DiagnosticSeverity defaultSeverity(DiagCode code) noexcept {
    switch (code) {
#define ENZAN_DIAG(name, number, severity, format) \
    case DiagCode::name: return DiagnosticSeverity::severity;
#include "common/DiagnosticCodes.def"
#undef ENZAN_DIAG
    }

    // Reached only if a value outside the catalogue was cast to DiagCode, which
    // is a defect in the caller rather than something a user can cause.
    return DiagnosticSeverity::Error;
}

std::string_view formatString(DiagCode code) noexcept {
    switch (code) {
#define ENZAN_DIAG(name, number, severity, format) \
    case DiagCode::name: return format;
#include "common/DiagnosticCodes.def"
#undef ENZAN_DIAG
    }

    return "unknown diagnostic";
}

// ---------------------------------------------------------------------------
// DiagnosticBuilder
// ---------------------------------------------------------------------------

DiagnosticBuilder::DiagnosticBuilder(DiagnosticEngine& owner, Diagnostic value)
    : engine(&owner), diagnostic(std::move(value)) {}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder&& other) noexcept
    : engine(other.engine), diagnostic(std::move(other.diagnostic)) {
    // The moved-from builder must not also report, or every diagnostic passed
    // through a function boundary would appear twice.
    other.engine = nullptr;
}

DiagnosticBuilder::~DiagnosticBuilder() {
    if (engine != nullptr) engine->emit(diagnostic);
}

DiagnosticBuilder& DiagnosticBuilder::note(SourceLocation location, std::string message) {
    if (engine != nullptr)
        diagnostic.notes.push_back({.location = location, .message = std::move(message)});
    return *this;
}

DiagnosticBuilder& DiagnosticBuilder::highlight(SourceRange range) {
    if (engine != nullptr) diagnostic.range = range;
    return *this;
}

// ---------------------------------------------------------------------------
// DiagnosticEngine
// ---------------------------------------------------------------------------

DiagnosticEngine::DiagnosticEngine(const SourceManager& sourceManager) noexcept
    : sources(sourceManager) {}

void DiagnosticEngine::addConsumer(DiagnosticConsumer& consumer) {
    consumers.push_back(&consumer);
}

void DiagnosticEngine::removeConsumer(DiagnosticConsumer& consumer) {
    const auto position = std::ranges::find(consumers, &consumer);
    if (position != consumers.end()) consumers.erase(position);
}

bool DiagnosticEngine::setSeverity(DiagCode code, DiagnosticSeverity severity) {
    if (defaultSeverity(code) == DiagnosticSeverity::Fatal) return false;

    overrides[code] = severity;
    return true;
}

bool DiagnosticEngine::suppress(DiagCode code) {
    return setSeverity(code, DiagnosticSeverity::Ignored);
}

void DiagnosticEngine::clearSeverity(DiagCode code) {
    overrides.erase(code);
}

DiagnosticSeverity DiagnosticEngine::severity(DiagCode code) const {
    const auto override = overrides.find(code);
    return override != overrides.end() ? override->second : defaultSeverity(code);
}

void DiagnosticEngine::setErrorLimit(unsigned newLimit) noexcept {
    limit = newLimit;
}

unsigned DiagnosticEngine::count(DiagnosticSeverity severity) const noexcept {
    return counts[static_cast<std::size_t>(severity)];
}

unsigned DiagnosticEngine::errorCount() const noexcept {
    return count(DiagnosticSeverity::Error) + count(DiagnosticSeverity::Fatal);
}

unsigned DiagnosticEngine::warningCount() const noexcept {
    return count(DiagnosticSeverity::Warning);
}

void DiagnosticEngine::reset() noexcept {
    counts.fill(0);
    limitReached = false;
}

std::string DiagnosticEngine::formatMessage(DiagCode code, std::format_args args) {
    try {
        return std::vformat(formatString(code), args);
    } catch (const std::format_error&) {
        // The catalogue and the call site disagree about the arguments. That is a
        // defect in Enzan, but it surfaces while something else is already going
        // wrong, so the diagnostic being reported must still reach the user.
        return std::format("[{} was reported with arguments that do not match its message]",
                           toString(code));
    }
}

void DiagnosticEngine::emit(const Diagnostic& diagnostic) {
    if (limitReached) return;

    ENZAN_ASSERT(diagnostic.severity != DiagnosticSeverity::Ignored,
                 "a suppressed diagnostic must never reach emit");

    ++counts[static_cast<std::size_t>(diagnostic.severity)];
    deliver(diagnostic);

    if (limit != 0 && errorCount() >= limit) {
        limitReached = true;

        Diagnostic overflow;
        overflow.code = DiagCode::TooManyErrors;
        overflow.severity = defaultSeverity(DiagCode::TooManyErrors);
        overflow.message = formatMessage(DiagCode::TooManyErrors, std::make_format_args(limit));

        // Counted and delivered directly: routing it through emit would re-enter
        // the limit check it has just triggered.
        ++counts[static_cast<std::size_t>(overflow.severity)];
        deliver(overflow);
    }
}

void DiagnosticEngine::deliver(const Diagnostic& diagnostic) {
    for (DiagnosticConsumer* consumer : consumers) consumer->consume(diagnostic);
}

// ---------------------------------------------------------------------------
// TextDiagnosticConsumer
// ---------------------------------------------------------------------------

namespace {

/// Expands tabs in `line` and reports where a given byte column lands.
///
/// Returns the expanded text. `byteColumn` is 1-based; the display column it maps
/// to, 0-based, is written to `displayColumn`. A byte column at or past the end
/// of the line maps to the end of the expanded text, which is what a caret
/// pointing at end-of-line needs.
std::string expandTabs(std::string_view line, std::uint32_t byteColumn, unsigned tabWidth,
                       std::size_t& displayColumn) {
    std::string expanded;
    expanded.reserve(line.size());

    const std::size_t target = byteColumn > 0 ? byteColumn - 1 : 0;
    displayColumn = 0;
    bool found = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        if (index == target) {
            displayColumn = expanded.size();
            found = true;
        }

        if (line[index] == '\t') {
            const std::size_t pad = tabWidth - (expanded.size() % tabWidth);
            expanded.append(pad, ' ');
        } else {
            expanded.push_back(line[index]);
        }
    }

    if (!found) displayColumn = expanded.size();

    return expanded;
}

} // namespace

TextDiagnosticConsumer::TextDiagnosticConsumer(const SourceManager& sourceManager,
                                               std::ostream& stream) noexcept
    : sources(sourceManager), output(stream) {}

void TextDiagnosticConsumer::consume(const Diagnostic& diagnostic) {
    writeHeader(diagnostic);
    if (snippets) writeSnippet(diagnostic.location, diagnostic.range);

    for (const DiagnosticNote& note : diagnostic.notes) {
        // Notes carry no code of their own: the code identifies the diagnostic,
        // and repeating it on every note would be noise in the transcript.
        if (note.location.valid()) {
            output << sources.bufferName(note.location.buffer()) << ':'
                   << sources.lineNumber(note.location) << ':'
                   << sources.columnNumber(note.location) << ": ";
        } else {
            output << "enzan: ";
        }
        output << toString(DiagnosticSeverity::Note) << ": " << note.message << '\n';

        if (snippets) writeSnippet(note.location, {});
    }
}

void TextDiagnosticConsumer::writeHeader(const Diagnostic& diagnostic) {
    if (diagnostic.location.valid()) {
        output << sources.bufferName(diagnostic.location.buffer()) << ':'
               << sources.lineNumber(diagnostic.location) << ':'
               << sources.columnNumber(diagnostic.location) << ": ";
    } else {
        // Without a position the tool's own name takes the place of the file, so
        // that every diagnostic line still starts with something identifying.
        output << "enzan: ";
    }

    output << toString(diagnostic.severity) << ": " << diagnostic.message << " ["
           << toString(diagnostic.code) << "]\n";
}

void TextDiagnosticConsumer::writeSnippet(SourceLocation location, SourceRange range) {
    if (!location.valid()) return;

    const std::uint32_t lineNumber = sources.lineNumber(location);
    if (lineNumber == 0) return;

    const std::string_view lineContent = sources.lineText(location);
    const std::uint32_t column = sources.columnNumber(location);

    std::size_t caretColumn = 0;
    const std::string expanded = expandTabs(lineContent, column, TabWidth, caretColumn);

    // The underline extends to the end of the highlighted range, clipped to this
    // line: a range spanning several lines is underlined only on the first, which
    // keeps the snippet to a fixed three lines.
    std::size_t underline = 0;
    if (range.valid() && range.start().buffer() == location.buffer() && range.length() > 1) {
        const SourceLocation lineStart = sources.lineStart(location.buffer(), lineNumber);
        const std::uint32_t lineEndOffset =
            lineStart.offset() + static_cast<std::uint32_t>(lineContent.size());
        const std::uint32_t endOffset = std::min(range.end().offset(), lineEndOffset);

        if (endOffset > location.offset()) {
            std::size_t endColumn = 0;
            expandTabs(lineContent, endOffset - lineStart.offset() + 1, TabWidth, endColumn);
            if (endColumn > caretColumn + 1) underline = endColumn - caretColumn - 1;
        }
    }

    const std::string label = std::to_string(lineNumber);
    const std::size_t gutter = std::max<std::size_t>(label.size(), 4);

    output << std::string(gutter - label.size(), ' ') << label << " | " << expanded << '\n';
    output << std::string(gutter, ' ') << " | " << std::string(caretColumn, ' ') << '^'
           << std::string(underline, '~') << '\n';
}

} // namespace enzan
