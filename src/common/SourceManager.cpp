// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/SourceManager.h"

#include "common/Assert.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <ios>
#include <ranges>
#include <system_error>
#include <utility>

namespace enzan {

namespace {

/// Maps a filesystem error to a read status.
///
/// The distinction matters for the diagnostic the caller emits: "no such file"
/// and "permission denied" lead a user to different fixes, and collapsing them
/// into "could not read" is the kind of unhelpful message this project is
/// trying not to produce.
FileReadStatus statusFor(const std::error_code& error) noexcept {
    if (error == std::errc::no_such_file_or_directory) return FileReadStatus::NotFound;
    if (error == std::errc::permission_denied) return FileReadStatus::PermissionDenied;
    return FileReadStatus::ReadFailed;
}

} // namespace

BufferId SourceManager::addBuffer(std::string name, std::string text) {
    if (text.size() > MaxBufferSize) return {};

    // The index is bounded by MaxBufferSize-many buffers long before it could
    // overflow, so the narrowing is safe; it is written out rather than left
    // implicit because -Wconversion is on for a reason.
    const auto index = static_cast<std::uint32_t>(buffers.size());

    auto buffer = std::make_unique<Buffer>();
    buffer->name = std::move(name);
    buffer->text = std::move(text);
    buffers.push_back(std::move(buffer));

    return BufferId::fromIndex(index);
}

FileReadResult SourceManager::readFile(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status fileStatus = std::filesystem::status(path, error);
    if (error) return {.buffer = {}, .status = statusFor(error), .systemMessage = error.message()};

    // A directory opens successfully on Linux and then fails to read, which
    // yields a confusing message; checking the type first produces a precise one.
    if (!std::filesystem::is_regular_file(fileStatus)) {
        return {.buffer = {},
                .status = FileReadStatus::NotARegularFile,
                .systemMessage = "not a regular file"};
    }

    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) return {.buffer = {}, .status = statusFor(error), .systemMessage = error.message()};

    if (size > MaxBufferSize) {
        return {.buffer = {},
                .status = FileReadStatus::TooLarge,
                .systemMessage = "source file exceeds the addressable size limit"};
    }

    // Read permission cannot be established from the file's metadata: ownership,
    // supplementary groups and ACLs all affect the answer, and `status` above
    // succeeds for a file the process may not read. The open is therefore the
    // test, and errno is what distinguishes "not permitted" from "went wrong".
    errno = 0;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        if (errno != 0) {
            const std::error_code openError(errno, std::generic_category());
            return {
                .buffer = {}, .status = statusFor(openError), .systemMessage = openError.message()};
        }
        return {.buffer = {},
                .status = FileReadStatus::ReadFailed,
                .systemMessage = "could not be opened for reading"};
    }

    // The file is read in one go into a pre-sized string. The size from
    // file_size is a hint rather than a guarantee — a file can shrink between
    // the two calls — so the string is resized to what was actually read.
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (stream.bad()) {
        return {.buffer = {},
                .status = FileReadStatus::ReadFailed,
                .systemMessage = "read failed before the end of the file"};
    }
    text.resize(static_cast<std::size_t>(stream.gcount()));

    const BufferId buffer = addBuffer(path.string(), std::move(text));
    ENZAN_ASSERT(buffer.valid(), "a file within the size limit must produce a valid buffer");

    return {.buffer = buffer, .status = FileReadStatus::Success, .systemMessage = {}};
}

std::string_view SourceManager::bufferName(BufferId id) const noexcept {
    const Buffer* buffer = find(id);
    return buffer != nullptr ? std::string_view(buffer->name) : std::string_view();
}

std::string_view SourceManager::bufferText(BufferId id) const noexcept {
    const Buffer* buffer = find(id);
    return buffer != nullptr ? std::string_view(buffer->text) : std::string_view();
}

std::uint32_t SourceManager::lineNumber(SourceLocation location) const noexcept {
    const Buffer* buffer = find(location.buffer());
    if (buffer == nullptr) return 0;

    const std::vector<std::uint32_t>& offsets = lineOffsets(*buffer);

    // upper_bound gives the first line starting after the offset, so the line
    // containing it is the one before. An offset past the end of the buffer —
    // which a truncated or malformed location can produce — lands on the last
    // line rather than reading out of range.
    const auto above = std::ranges::upper_bound(offsets, location.offset());
    return static_cast<std::uint32_t>(above - offsets.begin());
}

std::uint32_t SourceManager::columnNumber(SourceLocation location) const noexcept {
    const Buffer* buffer = find(location.buffer());
    if (buffer == nullptr) return 0;

    const std::uint32_t line = lineNumber(location);
    ENZAN_ASSERT(line > 0, "a valid location must lie on a line");

    const std::vector<std::uint32_t>& offsets = lineOffsets(*buffer);
    const std::uint32_t start = offsets[line - 1];
    const auto limit = static_cast<std::uint32_t>(buffer->text.size());
    const std::uint32_t offset = std::min(location.offset(), limit);

    return offset - start + 1;
}

std::string_view SourceManager::lineText(SourceLocation location) const noexcept {
    const Buffer* buffer = find(location.buffer());
    if (buffer == nullptr) return {};

    const std::uint32_t line = lineNumber(location);
    const std::vector<std::uint32_t>& offsets = lineOffsets(*buffer);
    const std::size_t start = offsets[line - 1];
    const std::size_t end = line < offsets.size() ? offsets[line] : buffer->text.size();

    std::string_view text(buffer->text);
    text = text.substr(start, end - start);

    // Strip the terminator so that callers never have to, and so that a `\r\n`
    // file does not put a stray carriage return in the middle of a diagnostic.
    if (text.ends_with('\n')) text.remove_suffix(1);
    if (text.ends_with('\r')) text.remove_suffix(1);

    return text;
}

SourceLocation SourceManager::lineStart(BufferId id, std::uint32_t line) const noexcept {
    const Buffer* buffer = find(id);
    if (buffer == nullptr || line == 0) return {};

    const std::vector<std::uint32_t>& offsets = lineOffsets(*buffer);
    if (line > offsets.size()) return {};

    return {id, offsets[line - 1]};
}

std::uint32_t SourceManager::lineCount(BufferId id) const noexcept {
    const Buffer* buffer = find(id);
    if (buffer == nullptr) return 0;

    return static_cast<std::uint32_t>(lineOffsets(*buffer).size());
}

const SourceManager::Buffer* SourceManager::find(BufferId id) const noexcept {
    if (!id.valid() || id.index() >= buffers.size()) return nullptr;
    return buffers[id.index()].get();
}

const std::vector<std::uint32_t>& SourceManager::lineOffsets(const Buffer& buffer) {
    // A buffer always has at least one line, so a non-empty index means it has
    // already been built and no separate "computed" flag is needed.
    if (buffer.lineOffsets.empty()) {
        buffer.lineOffsets.push_back(0);
        for (std::size_t offset = 0; offset < buffer.text.size(); ++offset) {
            if (buffer.text[offset] != '\n') continue;

            // A newline that terminates the buffer does not start a further
            // line. Counting one would give a three-line file that ends in a
            // newline — which is every well-formed source file — a phantom
            // fourth line, and the Source window would show an empty row at the
            // end of every file.
            const std::size_t next = offset + 1;
            if (next == buffer.text.size()) break;

            buffer.lineOffsets.push_back(static_cast<std::uint32_t>(next));
        }
    }
    return buffer.lineOffsets;
}

} // namespace enzan
