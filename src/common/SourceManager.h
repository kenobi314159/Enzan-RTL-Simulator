// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {

/// Why a source file could not be turned into a buffer.
///
/// The cause is returned as an enumerator rather than only as a message so that
/// the caller can map it to a stable diagnostic code (DESIGN_GUIDE.md section
/// 10.6). The human-readable system message is carried alongside it.
enum class FileReadStatus : std::uint8_t {
    Success,
    NotFound,
    NotARegularFile,
    PermissionDenied,
    ReadFailed,
    TooLarge,
};

/// Outcome of SourceManager::readFile.
struct FileReadResult {
    /// The buffer holding the file's text. Valid if and only if
    /// `status == FileReadStatus::Success`.
    BufferId buffer;

    FileReadStatus status = FileReadStatus::ReadFailed;

    /// The operating system's description of the failure, empty on success.
    /// Intended to be attached to a diagnostic so that the user sees the real
    /// reason rather than a generic "could not read".
    std::string systemMessage;

    [[nodiscard]] bool ok() const noexcept { return status == FileReadStatus::Success; }
};

/// Owns the text of every source file the tool has read, and maps byte offsets
/// back to human-readable positions.
///
/// Every component that produces a diagnostic, a waveform scope name or a
/// source-window position refers to source text through this class rather than
/// holding file paths and re-reading files. That keeps one copy of each file in
/// memory and makes a SourceLocation meaningful anywhere in the tool.
///
/// **Lifetime.** Buffer text is owned by the manager and is never moved or freed
/// while the manager lives, so a `std::string_view` returned by `bufferText` or
/// `lineText` stays valid for as long as the manager does. Locations from one
/// manager must not be used with another.
///
/// **Thread safety.** Not thread-safe, not even for concurrent `const` access:
/// the line index is built lazily on first query and cached. Parallel analysis
/// (DESIGN_GUIDE.md section 6.4) must therefore either use one manager per
/// worker or serialise access; the constraint is recorded here because it is
/// easy to violate accidentally once analysis is parallelised.
class SourceManager {
public:
    SourceManager() = default;

    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;
    SourceManager(SourceManager&&) noexcept = default;
    SourceManager& operator=(SourceManager&&) noexcept = default;
    ~SourceManager() = default;

    /// The largest source buffer that can be addressed.
    ///
    /// A SourceLocation holds a 32-bit offset, so a buffer must be smaller than
    /// 2^32 bytes. The limit is exposed, and checked on every read, because a
    /// silently truncated source file would produce diagnostics pointing at the
    /// wrong lines — worse than a clean refusal.
    static constexpr std::size_t MaxBufferSize = 0xFFFF'FFFEU;

    /// Adds a buffer holding already-available text and returns its identifier.
    ///
    /// `name` is what diagnostics display; it is usually a path but may be any
    /// label, which is how command-line macro definitions and GUI-entered
    /// expressions get source positions. Text longer than `MaxBufferSize` is
    /// rejected by returning an invalid BufferId; callers that construct text
    /// themselves are expected not to hit it.
    BufferId addBuffer(std::string name, std::string text);

    /// Reads `path` and adds its contents as a buffer.
    ///
    /// The file is read as bytes with no encoding conversion and no line-ending
    /// translation, so offsets correspond exactly to the bytes on disk. Failure
    /// is reported in the returned result rather than by throwing, because a
    /// missing source file is an ordinary user error that becomes a diagnostic,
    /// not an exceptional condition (DESIGN_GUIDE.md section 13.2).
    ///
    /// The same path read twice produces two independent buffers; the manager
    /// does not deduplicate, because the caller is better placed to decide
    /// whether two references to a path are the same translation unit.
    FileReadResult readFile(const std::filesystem::path& path);

    /// Returns the number of buffers held.
    [[nodiscard]] std::size_t bufferCount() const noexcept { return buffers.size(); }

    /// Returns the display name of `id`, or an empty view if `id` is invalid.
    [[nodiscard]] std::string_view bufferName(BufferId id) const noexcept;

    /// Returns the full text of `id`, or an empty view if `id` is invalid.
    [[nodiscard]] std::string_view bufferText(BufferId id) const noexcept;

    /// Returns the 1-based line number containing `location`, or 0 if the
    /// location is invalid.
    ///
    /// A line break is `\n` or `\r\n`. A lone `\r` is not treated as a break: no
    /// HDL toolchain in use produces classic-Mac line endings, and guessing
    /// would misreport positions in files that contain a bare `\r` in a string
    /// literal.
    [[nodiscard]] std::uint32_t lineNumber(SourceLocation location) const noexcept;

    /// Returns the 1-based column of `location`, or 0 if the location is
    /// invalid.
    ///
    /// Columns count bytes, and a tab counts as one column. A renderer that
    /// draws a caret under a line therefore has to expand tabs itself to keep
    /// the caret aligned; TextDiagnosticConsumer does exactly that.
    [[nodiscard]] std::uint32_t columnNumber(SourceLocation location) const noexcept;

    /// Returns the text of the line containing `location`, without its line
    /// terminator, or an empty view if the location is invalid.
    [[nodiscard]] std::string_view lineText(SourceLocation location) const noexcept;

    /// Returns the location of the first byte of the 1-based `line` in `id`, or
    /// an invalid location if `id` is invalid or the line does not exist.
    [[nodiscard]] SourceLocation lineStart(BufferId id, std::uint32_t line) const noexcept;

    /// Returns the number of lines in `id`, or 0 if `id` is invalid.
    ///
    /// A buffer with no line breaks has one line, including when it is empty. A
    /// newline that terminates the buffer does not begin a further line, so
    /// `"a\nb\nc\n"` has three lines, matching how editors count. A location at
    /// the very end of such a buffer therefore reports the last line rather than
    /// a line past the end.
    [[nodiscard]] std::uint32_t lineCount(BufferId id) const noexcept;

private:
    struct Buffer {
        std::string name;
        std::string text;

        /// Byte offset of the first character of each line, `lineOffsets[0] == 0`.
        /// Built on first use rather than at read time: most buffers never have a
        /// diagnostic reported against them, and indexing every file up front is
        /// pure cost on the common path.
        mutable std::vector<std::uint32_t> lineOffsets;
    };

    /// Returns the buffer for `id`, or nullptr when `id` is invalid or out of
    /// range. Every public accessor goes through this so that an invalid
    /// identifier degrades to an empty result instead of reading out of bounds:
    /// diagnostics are often produced on error paths where a location may not
    /// have been established, and crashing while reporting an error is the worst
    /// possible behaviour.
    [[nodiscard]] const Buffer* find(BufferId id) const noexcept;

    /// Returns the line index for `buffer`, building it on first call.
    static const std::vector<std::uint32_t>& lineOffsets(const Buffer& buffer);

    /// Buffers are held indirectly so that adding one never invalidates a
    /// `string_view` handed out earlier: growing the vector would move the
    /// `std::string` objects, and a short string keeps its bytes inside itself.
    std::vector<std::unique_ptr<Buffer>> buffers;
};

/// Finds the buffer holding `file` among those `sources` holds.
///
/// Exactly by name first: a user who named a file the way the design did means that file, and
/// nothing should come between them and it. Failing that, by the tail of the path, so that
/// `counter.sv` finds `/home/who/rtl/counter.sv` — a file list expands to absolute paths, and
/// making a user type one out in full to set a breakpoint would be friction with nothing
/// behind it.
///
/// Returns an invalid identifier when nothing matches, and also when several *differently
/// named* buffers match the tail, filling `ambiguity` with their names when it is given. One
/// file legitimately has more than one buffer — the manager does not deduplicate — so buffers
/// sharing a name are not an ambiguity.
///
/// Lives here rather than in a caller because both ends of the tool need it: the control
/// layer resolves `bp counter.sv 19`, and the kernel resolves the same request arriving from a
/// front end. Two copies of a matching rule would drift, and the two would then disagree about
/// which file a breakpoint is on.
[[nodiscard]] BufferId findBufferByName(const SourceManager& sources, std::string_view file,
                                        std::string* ambiguity = nullptr);

} // namespace enzan
