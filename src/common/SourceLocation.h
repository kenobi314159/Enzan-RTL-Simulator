// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <compare>
#include <cstdint>

namespace enzan {

/// Identifies one source buffer held by a SourceManager.
///
/// A default-constructed BufferId is invalid, which lets an absent buffer be
/// represented without an optional. Identifiers are assigned in the order
/// buffers are added, never derived from a pointer or a hash, so that anything
/// ordered by BufferId is ordered reproducibly — diagnostic output included
/// (DESIGN_GUIDE.md section 3.8).
class BufferId {
public:
    constexpr BufferId() noexcept = default;

    /// Constructs the identifier for the buffer at `index` in its manager.
    static constexpr BufferId fromIndex(std::uint32_t index) noexcept {
        BufferId result;
        result.value = index + 1;
        return result;
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

    /// Returns the zero-based index of this buffer in its manager.
    ///
    /// Only meaningful when `valid()`; the value for an invalid identifier is
    /// unspecified rather than checked, because this sits on the diagnostic and
    /// waveform paths where the caller has already established validity.
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return value - 1; }

    friend constexpr bool operator==(BufferId, BufferId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(BufferId, BufferId) noexcept = default;

private:
    /// One greater than the buffer index, so that zero means invalid.
    std::uint32_t value = 0;
};

/// A byte offset within one source buffer.
///
/// Offsets are byte offsets, not character offsets: the frontends work on bytes
/// and UTF-8 source is passed through unchanged, so a column number counts bytes
/// too. That is documented on SourceManager::columnNumber because it is
/// observable in diagnostic output.
///
/// A default-constructed SourceLocation is invalid and denotes "no position",
/// which is what a diagnostic about the command line or about the design as a
/// whole carries.
class SourceLocation {
public:
    constexpr SourceLocation() noexcept = default;

    constexpr SourceLocation(BufferId buffer, std::uint32_t offset) noexcept
        : bufferValue(buffer), offsetValue(offset) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return bufferValue.valid(); }

    [[nodiscard]] constexpr BufferId buffer() const noexcept { return bufferValue; }

    [[nodiscard]] constexpr std::uint32_t offset() const noexcept { return offsetValue; }

    /// Returns this location moved forward by `delta` bytes within the same
    /// buffer. Used to point at a position derived from a token start.
    [[nodiscard]] constexpr SourceLocation advanced(std::uint32_t delta) const noexcept {
        return {bufferValue, offsetValue + delta};
    }

    friend constexpr bool operator==(SourceLocation, SourceLocation) noexcept = default;

    /// Orders by buffer first, then by offset, so that sorting a list of
    /// locations groups them by file in the order the files were added.
    friend constexpr std::strong_ordering operator<=>(SourceLocation,
                                                      SourceLocation) noexcept = default;

private:
    BufferId bufferValue;
    std::uint32_t offsetValue = 0;
};

/// A half-open range `[start, end)` of bytes within one source buffer.
///
/// Both endpoints must lie in the same buffer; a range that spans buffers is not
/// representable, because nothing in the tool has a use for one and allowing it
/// would make every consumer handle a case that cannot be rendered.
class SourceRange {
public:
    constexpr SourceRange() noexcept = default;

    constexpr SourceRange(SourceLocation start, SourceLocation end) noexcept
        : startValue(start), endValue(end) {}

    [[nodiscard]] constexpr SourceLocation start() const noexcept { return startValue; }

    [[nodiscard]] constexpr SourceLocation end() const noexcept { return endValue; }

    /// True when both endpoints are valid, lie in the same buffer, and `start`
    /// does not follow `end`. An empty range with `start == end` is valid and
    /// denotes a zero-width position, as produced by an insertion suggestion.
    [[nodiscard]] constexpr bool valid() const noexcept {
        return startValue.valid() && endValue.valid() && startValue.buffer() == endValue.buffer()
               && startValue.offset() <= endValue.offset();
    }

    /// Returns the number of bytes covered, or zero when the range is not valid.
    [[nodiscard]] constexpr std::uint32_t length() const noexcept {
        return valid() ? endValue.offset() - startValue.offset() : 0;
    }

    friend constexpr bool operator==(SourceRange, SourceRange) noexcept = default;

private:
    SourceLocation startValue;
    SourceLocation endValue;
};

} // namespace enzan
