// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/FileList.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace enzan::control {

namespace {

/// One word of a file list, with where it came from.
struct Token {
    std::string text;
    SourceLocation location;
};

/// How a nested list's relative paths are resolved.
enum class Anchor : std::uint8_t {
    /// `-f`: against the process's working directory, which is what the file was written
    /// against in every tool that reads these.
    WorkingDirectory,
    /// `-F`: against the directory of the list that named it.
    ListDirectory,
};

/// Splits a file list buffer into words, resolving comments, quotes and `$VAR`.
///
/// A class rather than a function because the three failure modes — an unterminated comment, an
/// unterminated quote and an unset variable — each need the position of the thing that went
/// wrong, and threading that through a chain of free functions is worse than holding it.
class Tokenizer {
public:
    Tokenizer(BufferId sourceBuffer, std::string_view content, DiagnosticEngine& engine)
        : buffer(sourceBuffer), text(content), diagnostics(engine) {}

    /// Returns the words, or nothing having reported why.
    bool tokenize(std::vector<Token>& out) {
        while (skipWhitespaceAndComments()) {
            Token token;
            token.location = at(position);
            if (!readWord(token.text)) return false;
            if (!token.text.empty()) out.push_back(std::move(token));
        }
        return !failed;
    }

private:
    [[nodiscard]] SourceLocation at(std::size_t offset) const {
        return {buffer, static_cast<std::uint32_t>(offset)};
    }

    [[nodiscard]] bool atEnd() const noexcept { return position >= text.size(); }

    /// Advances past whitespace and comments. Returns false at the end of the buffer or on a
    /// failure, which `failed` distinguishes.
    bool skipWhitespaceAndComments() {
        while (!atEnd()) {
            const char character = text[position];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                ++position;
                continue;
            }

            if (text.compare(position, 2, "//") == 0) {
                position = text.find('\n', position);
                if (position == std::string_view::npos) position = text.size();
                continue;
            }

            if (text.compare(position, 2, "/*") == 0) {
                const std::size_t start = position;
                const std::size_t end = text.find("*/", position + 2);
                if (end == std::string_view::npos) {
                    diagnostics.report(DiagCode::FileListUnterminatedComment, at(start));
                    failed = true;
                    return false;
                }
                position = end + 2;
                continue;
            }

            return true;
        }
        return false;
    }

    /// Reads one word, honouring double quotes and substituting environment variables.
    bool readWord(std::string& out) {
        while (!atEnd()) {
            const char character = text[position];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n')
                break;
            if (text.compare(position, 2, "//") == 0 || text.compare(position, 2, "/*") == 0) break;

            if (character == '"') {
                if (!readQuoted(out)) return false;
                continue;
            }

            if (character == '$') {
                if (!readVariable(out)) return false;
                continue;
            }

            out += character;
            ++position;
        }
        return true;
    }

    bool readQuoted(std::string& out) {
        const std::size_t start = position;
        ++position; // the opening quote
        while (!atEnd() && text[position] != '"') {
            if (text[position] == '\n') break;
            if (text[position] == '$') {
                if (!readVariable(out)) return false;
                continue;
            }
            out += text[position];
            ++position;
        }

        if (atEnd() || text[position] != '"') {
            diagnostics.report(DiagCode::FileListUnterminatedQuote, at(start));
            failed = true;
            return false;
        }

        ++position; // the closing quote
        return true;
    }

    /// Substitutes `$NAME` or `${NAME}` from the environment.
    ///
    /// Unset is an error rather than an empty expansion. File lists name paths, and an empty
    /// expansion turns `$RTL_ROOT/cpu.sv` into `/cpu.sv` — a file that does not exist, reported
    /// as a missing source rather than as the missing variable that caused it.
    bool readVariable(std::string& out) {
        const std::size_t start = position;
        ++position; // the '$'

        const bool braced = !atEnd() && text[position] == '{';
        if (braced) ++position;

        const std::size_t nameStart = position;
        while (!atEnd()) {
            const char character = text[position];
            const bool part = (character >= 'A' && character <= 'Z')
                              || (character >= 'a' && character <= 'z')
                              || (character >= '0' && character <= '9') || character == '_';
            if (!part) break;
            ++position;
        }

        const std::string name(text.substr(nameStart, position - nameStart));
        if (braced) {
            if (atEnd() || text[position] != '}') {
                diagnostics.report(DiagCode::FileListUnterminatedBrace, at(start));
                failed = true;
                return false;
            }
            ++position;
        }

        if (name.empty()) {
            // A lone `$` is a literal dollar, which is what a path containing one means.
            out += '$';
            return true;
        }

        const char* value = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe)
        if (value == nullptr) {
            diagnostics.report(DiagCode::EnvironmentVariableNotSet, at(start), name);
            failed = true;
            return false;
        }

        out += value;
        return true;
    }

    BufferId buffer;
    std::string_view text;
    DiagnosticEngine& diagnostics;
    std::size_t position = 0;
    bool failed = false;
};

/// Splits `+incdir+a+b+` into its parts, dropping the empty ones a trailing `+` produces.
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

/// Reads one list, with the stack of lists that led here for cycle reporting.
class Reader {
public:
    Reader(SourceManager& sourceManager, DiagnosticEngine& engine, FileList& out)
        : sources(sourceManager), diagnostics(engine), result(out) {}

    bool read(const std::filesystem::path& path, Anchor anchor,
              const std::filesystem::path& parentDirectory) {
        const std::filesystem::path resolved = resolve(path, anchor, parentDirectory);

        // Canonical, so that `./list.f` and `list.f` are recognised as the same file. A path
        // that cannot be canonicalised is one that does not exist, which the read below
        // reports properly; the uncanonicalised form is used for the cycle check meanwhile.
        std::error_code failure;
        std::filesystem::path key = std::filesystem::weakly_canonical(resolved, failure);
        if (failure) key = resolved;

        for (const std::filesystem::path& open : stack) {
            if (open != key) continue;

            std::string cycle;
            for (const std::filesystem::path& entry : stack) {
                if (!cycle.empty()) cycle += " -> ";
                cycle += entry.string();
            }
            cycle += " -> " + key.string();
            diagnostics.report(DiagCode::FileListCycle, SourceLocation(), resolved.string(), cycle);
            return false;
        }

        const FileReadResult read = sources.readFile(resolved);
        if (!read.ok()) {
            reportFileReadFailure(diagnostics, resolved, read);
            return false;
        }

        std::vector<Token> tokens;
        Tokenizer tokenizer(read.buffer, sources.bufferText(read.buffer), diagnostics);
        if (!tokenizer.tokenize(tokens)) return false;

        stack.push_back(key);
        const bool ok = process(tokens, resolved.parent_path());
        stack.pop_back();
        return ok;
    }

private:
    static std::filesystem::path resolve(const std::filesystem::path& path, Anchor anchor,
                                         const std::filesystem::path& parentDirectory) {
        if (path.is_absolute() || anchor == Anchor::WorkingDirectory) return path;
        if (parentDirectory.empty()) return path;
        return parentDirectory / path;
    }

    /// Consumes the token after `index`, or reports that the option was left dangling.
    const Token* value(const std::vector<Token>& tokens, std::size_t& index) {
        if (index + 1 >= tokens.size()) {
            diagnostics.report(DiagCode::FileListMissingValue, tokens[index].location,
                               tokens[index].text);
            return nullptr;
        }
        return &tokens[++index];
    }

    bool process(const std::vector<Token>& tokens, const std::filesystem::path& directory) {
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            const Token& token = tokens[index];
            const std::string_view text = token.text;

            if (text == "-f" || text == "-F") {
                const Token* nested = value(tokens, index);
                if (nested == nullptr) return false;
                const Anchor anchor =
                    text == "-F" ? Anchor::ListDirectory : Anchor::WorkingDirectory;
                if (!read(nested->text, anchor, directory)) return false;
                continue;
            }

            if (text == "-v") {
                const Token* file = value(tokens, index);
                if (file == nullptr) return false;
                result.libraryFiles.emplace_back(file->text);
                continue;
            }

            if (text == "-y") {
                const Token* dir = value(tokens, index);
                if (dir == nullptr) return false;
                result.libraryDirectories.emplace_back(dir->text);
                continue;
            }

            // `-sv` asks for the files after it to be read as SystemVerilog, which is the only
            // thing Enzan reads. Accepted and recorded nowhere rather than rejected, because
            // refusing a switch that asks for exactly what already happens would turn a
            // perfectly good file list away over nothing.
            if (text == "-sv") continue;

            if (text.starts_with("+incdir+")) {
                if (!appendPlusList(token, text.substr(8), result.includeDirectories)) return false;
                continue;
            }

            if (text.starts_with("+define+")) {
                for (const std::string_view part : splitPlusList(text.substr(8)))
                    result.defines.emplace_back(part);
                if (!checkNotEmpty(token, text.substr(8))) return false;
                continue;
            }

            if (text.starts_with("+libext+")) {
                for (const std::string_view part : splitPlusList(text.substr(8)))
                    result.libraryExtensions.emplace_back(part);
                if (!checkNotEmpty(token, text.substr(8))) return false;
                continue;
            }

            if (text.starts_with('-') || text.starts_with('+')) {
                diagnostics.report(DiagCode::FileListUnknownOption, token.location, text);
                return false;
            }

            result.files.emplace_back(text);
        }

        return true;
    }

    bool appendPlusList(const Token& token, std::string_view tail,
                        std::vector<std::filesystem::path>& out) {
        for (const std::string_view part : splitPlusList(tail)) out.emplace_back(part);
        return checkNotEmpty(token, tail);
    }

    bool checkNotEmpty(const Token& token, std::string_view tail) {
        if (!splitPlusList(tail).empty()) return true;
        diagnostics.report(DiagCode::FileListEmptyOption, token.location, token.text);
        return false;
    }

    SourceManager& sources;
    DiagnosticEngine& diagnostics;
    FileList& result;

    /// The lists currently open, outermost first. A vector rather than a set because it is
    /// never more than a handful deep and because reporting the cycle needs the order.
    std::vector<std::filesystem::path> stack;
};

} // namespace

bool readFileList(const std::filesystem::path& path, SourceManager& sources,
                  DiagnosticEngine& diagnostics, FileList& result) {
    Reader reader(sources, diagnostics, result);
    return reader.read(path, Anchor::WorkingDirectory, {});
}

} // namespace enzan::control
