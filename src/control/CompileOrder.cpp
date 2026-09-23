// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/CompileOrder.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace enzan::control {

namespace {

[[nodiscard]] bool isIdentifierStart(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
           || character == '_';
}

[[nodiscard]] bool isIdentifierPart(char character) noexcept {
    return isIdentifierStart(character) || (character >= '0' && character <= '9')
           || character == '$';
}

/// Maps a declaration keyword to what it declares.
[[nodiscard]] std::optional<UnitKind> declarationKeyword(std::string_view word) noexcept {
    if (word == "module" || word == "macromodule") return UnitKind::Module;
    if (word == "package") return UnitKind::Package;
    if (word == "interface") return UnitKind::Interface;
    if (word == "program") return UnitKind::Program;
    if (word == "primitive") return UnitKind::Primitive;
    if (word == "class") return UnitKind::Class;
    return std::nullopt;
}

/// Maps a closing keyword to what it closes.
[[nodiscard]] std::optional<UnitKind> closingKeyword(std::string_view word) noexcept {
    if (word == "endmodule") return UnitKind::Module;
    if (word == "endpackage") return UnitKind::Package;
    if (word == "endinterface") return UnitKind::Interface;
    if (word == "endprogram") return UnitKind::Program;
    if (word == "endprimitive") return UnitKind::Primitive;
    if (word == "endclass") return UnitKind::Class;
    return std::nullopt;
}

/// The single linear pass over one file's bytes.
class Scanner {
public:
    Scanner(BufferId sourceBuffer, std::string_view content)
        : buffer(sourceBuffer), text(content) {}

    SourceScan run() {
        SourceScan scan;
        std::string_view previous;

        while (skipTrivia()) {
            const std::size_t start = position;
            const std::string_view word = readIdentifier();
            if (word.empty()) {
                // Punctuation, a number, or anything else with no bearing on the graph.
                ++position;
                previous = {};
                continue;
            }

            // `pkg::name`, the reference that actually orders anything. Checked before the
            // keyword tests so that a package called `program` cannot confuse them.
            if (peekScopeOperator()) {
                scan.packageReferences.push_back(
                    {.name = std::string(word), .location = at(start)});
                position += 2;
                previous = {};
                continue;
            }

            if (word == "import" || word == "export") {
                previous = word;
                continue;
            }

            if (const std::optional<UnitKind> kind = declarationKeyword(word)) {
                // `virtual interface bus_if handle;` names a type and opens nothing; so does
                // `extern module m(...);`, which is a prototype with no body and therefore no
                // `endmodule` to close the frame it would otherwise open. `virtual class`, by
                // contrast, is an ordinary class declaration and is not excluded here.
                if ((previous == "virtual" && kind == UnitKind::Interface)
                    || previous == "extern") {
                    previous = word;
                    continue;
                }

                handleDeclaration(*kind, scan);
                previous = {};
                continue;
            }

            if (const std::optional<UnitKind> closing = closingKeyword(word)) {
                close(*closing);
                previous = {};
                continue;
            }

            previous = word;
        }

        return scan;
    }

private:
    [[nodiscard]] SourceLocation at(std::size_t offset) const {
        return {buffer, static_cast<std::uint32_t>(offset)};
    }

    [[nodiscard]] bool atEnd() const noexcept { return position >= text.size(); }

    /// Advances past whitespace, comments and string literals. Returns false at the end.
    bool skipTrivia() {
        while (!atEnd()) {
            const char character = text[position];
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                ++position;
                continue;
            }

            if (text.compare(position, 2, "//") == 0) {
                position = std::min(text.find('\n', position), text.size());
                continue;
            }

            if (text.compare(position, 2, "/*") == 0) {
                const std::size_t end = text.find("*/", position + 2);
                position = end == std::string_view::npos ? text.size() : end + 2;
                continue;
            }

            if (character == '"') {
                skipString();
                continue;
            }

            return true;
        }
        return false;
    }

    void skipString() {
        ++position; // the opening quote
        while (!atEnd()) {
            if (text[position] == '\\' && position + 1 < text.size()) {
                position += 2;
                continue;
            }
            if (text[position] == '"') {
                ++position;
                return;
            }
            ++position;
        }
    }

    /// Reads an identifier at the current position, or returns an empty view.
    ///
    /// An escaped identifier — `\bus[0] ` — is read whole, terminator included, because the
    /// name between the backslash and the space is what the source refers to it by.
    std::string_view readIdentifier() {
        if (text[position] == '\\') {
            const std::size_t start = position + 1;
            std::size_t end = start;
            while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n'
                   && text[end] != '\r')
                ++end;
            position = end;
            return text.substr(start, end - start);
        }

        if (!isIdentifierStart(text[position])) return {};

        const std::size_t start = position;
        while (position < text.size() && isIdentifierPart(text[position])) ++position;
        return text.substr(start, position - start);
    }

    /// True when the very next characters are `::`, with no space between.
    ///
    /// No space, because `pkg :: name` is legal and `pkg` on its own line followed by a
    /// continuation is not something a lexical scan can tell apart from the next statement
    /// starting with `::`, which cannot happen — so requiring adjacency costs a rare spelling
    /// and removes a class of false reference entirely.
    [[nodiscard]] bool peekScopeOperator() const { return text.compare(position, 2, "::") == 0; }

    void handleDeclaration(UnitKind kind, SourceScan& scan) {
        const bool atFileScope = open.empty();
        open.push_back(kind);

        if (!atFileScope) return;

        if (!skipTrivia()) return;
        const std::size_t start = position;
        const std::string_view name = readIdentifier();
        if (name.empty()) return;

        scan.declarations.push_back(
            {.kind = kind, .name = std::string(name), .location = at(start)});
    }

    /// Closes the innermost matching construct.
    ///
    /// Tolerant by design: it pops down to the nearest frame of the right kind rather than
    /// insisting the top one matches. A lexical scan opens frames it should not — a generic
    /// interface port reads as an `interface` keyword — and an `endmodule` that could not find
    /// its module would leave the rest of the file looking nested, losing every declaration
    /// after it. Popping down resynchronises instead.
    void close(UnitKind kind) {
        for (std::size_t depth = open.size(); depth > 0; --depth) {
            if (open[depth - 1] != kind) continue;
            open.resize(depth - 1);
            return;
        }
    }

    BufferId buffer;
    std::string_view text;
    std::size_t position = 0;

    /// The constructs currently open, outermost first.
    std::vector<UnitKind> open;
};

/// The dependency graph over the files being ordered.
class Graph {
public:
    explicit Graph(std::span<const std::filesystem::path> files)
        : paths(files.begin(), files.end()),
          successors(files.size()),
          predecessorCount(files.size(), 0) {}

    /// Records that `from` must be analysed before `to`, ignoring a repeat.
    void addEdge(std::size_t from, std::size_t to) {
        std::vector<std::size_t>& list = successors[from];
        if (std::ranges::find(list, to) != list.end()) return;
        list.push_back(to);
        ++predecessorCount[to];
    }

    /// Kahn's algorithm, always taking the lowest-numbered ready file.
    ///
    /// Lowest-numbered rather than whichever the queue offers, so that files with no
    /// dependency between them keep the order they were given. Analysis order is observable
    /// and section 3.8 makes observable ordering reproducible, which a hash-ordered or
    /// stack-ordered traversal would not be.
    [[nodiscard]] std::vector<std::size_t> sorted() const {
        std::vector<unsigned> remaining(predecessorCount.begin(), predecessorCount.end());
        std::vector<bool> emitted(paths.size(), false);
        std::vector<std::size_t> order;
        order.reserve(paths.size());

        for (std::size_t step = 0; step < paths.size(); ++step) {
            std::size_t next = paths.size();
            for (std::size_t index = 0; index < paths.size(); ++index) {
                if (emitted[index] || remaining[index] != 0) continue;
                next = index;
                break;
            }

            if (next == paths.size()) break; // a cycle: nothing is ready

            emitted[next] = true;
            order.push_back(next);
            for (const std::size_t successor : successors[next]) --remaining[successor];
        }

        return order;
    }

    /// Returns one cycle among the files not in `order`, as indices, first repeated last.
    [[nodiscard]] std::vector<std::size_t> findCycle(const std::vector<std::size_t>& order) const {
        std::vector<bool> placed(paths.size(), false);
        for (const std::size_t index : order) placed[index] = true;

        // Every file left over is on a cycle or downstream of one, so walking forward from any
        // of them reaches a cycle. Walking until a file repeats finds it.
        std::vector<std::size_t> walk;
        std::vector<int> visitedAt(paths.size(), -1);

        std::size_t current = paths.size();
        for (std::size_t index = 0; index < paths.size(); ++index) {
            if (placed[index]) continue;
            current = index;
            break;
        }
        if (current == paths.size()) return {};

        while (visitedAt[current] < 0) {
            visitedAt[current] = static_cast<int>(walk.size());
            walk.push_back(current);

            std::size_t next = paths.size();
            for (const std::size_t successor : successors[current]) {
                if (placed[successor]) continue;
                next = successor;
                break;
            }
            if (next == paths.size()) return {};
            current = next;
        }

        std::vector<std::size_t> cycle(walk.begin() + visitedAt[current], walk.end());
        cycle.push_back(current);
        return cycle;
    }

    [[nodiscard]] const std::filesystem::path& path(std::size_t index) const {
        return paths[index];
    }

    [[nodiscard]] std::size_t size() const noexcept { return paths.size(); }

private:
    std::vector<std::filesystem::path> paths;
    std::vector<std::vector<std::size_t>> successors;
    std::vector<unsigned> predecessorCount;
};

} // namespace

std::string_view toString(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::Module: return "module";
    case UnitKind::Package: return "package";
    case UnitKind::Interface: return "interface";
    case UnitKind::Program: return "program";
    case UnitKind::Primitive: return "primitive";
    case UnitKind::Class: return "class";
    }
    return "design unit";
}

SourceScan scanSystemVerilog(BufferId buffer, std::string_view text) {
    Scanner scanner(buffer, text);
    return scanner.run();
}

bool inferCompileOrder(std::span<const std::filesystem::path> files, SourceManager& sources,
                       DiagnosticEngine& diagnostics, std::vector<std::filesystem::path>& ordered) {
    ordered.clear();

    std::vector<SourceScan> scans;
    scans.reserve(files.size());
    for (const std::filesystem::path& file : files) {
        const FileReadResult read = sources.readFile(file);
        if (!read.ok()) {
            reportFileReadFailure(diagnostics, file, read);
            return false;
        }
        scans.push_back(scanSystemVerilog(read.buffer, sources.bufferText(read.buffer)));
    }

    // Ordered rather than hash-based, so that two files declaring the same package resolve the
    // same way on every run (section 3.8). The first declaration wins, matching the order the
    // user gave the files.
    std::map<std::string, std::size_t, std::less<>> packageOwner;
    for (std::size_t index = 0; index < scans.size(); ++index) {
        for (const Declaration& declaration : scans[index].declarations) {
            if (declaration.kind != UnitKind::Package) continue;
            packageOwner.emplace(declaration.name, index);
        }
    }

    Graph graph(files);
    for (std::size_t index = 0; index < scans.size(); ++index) {
        for (const PackageReference& reference : scans[index].packageReferences) {
            const auto owner = packageOwner.find(reference.name);

            // A name no scanned file declares as a package is not a package reference: it is a
            // class scope, an enum's type name, or a package from a library that is already
            // analysed. None of those orders anything here.
            if (owner == packageOwner.end() || owner->second == index) continue;
            graph.addEdge(owner->second, index);
        }
    }

    const std::vector<std::size_t> order = graph.sorted();
    if (order.size() != graph.size()) {
        const std::vector<std::size_t> cycle = graph.findCycle(order);
        std::string spelled;
        for (const std::size_t index : cycle) {
            if (!spelled.empty()) spelled += " -> ";
            spelled += graph.path(index).string();
        }
        diagnostics.report(DiagCode::CompileOrderCycle, SourceLocation(), spelled);
        return false;
    }

    ordered.reserve(order.size());
    for (const std::size_t index : order) ordered.push_back(graph.path(index));
    return true;
}

} // namespace enzan::control
