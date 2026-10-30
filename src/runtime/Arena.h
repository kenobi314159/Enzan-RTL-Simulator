// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/Assert.h"
#include "eir/BitVector.h"

#include <cstdint>
#include <span>
#include <vector>

namespace enzan::runtime {

/// The single contiguous allocation holding all mutable simulation state.
///
/// DESIGN_GUIDE.md section 4.3 requires exactly this, and the consequences are all
/// deliberate: checkpoint and restore become a memcpy of the arena; the waveform
/// database reads values straight out of it with no indirection; a JIT can burn offsets
/// into generated code as constants; and cache locality becomes something that can be
/// improved by reordering the layout rather than by chasing pointers.
///
/// Every access is by Word offset. Nothing in the simulator holds a pointer into the
/// arena across a step, because the arena may be reallocated between runs and because a
/// stored pointer is what makes checkpointing impossible.
class Arena {
public:
    Arena() = default;

    explicit Arena(std::uint32_t wordCount) { resize(wordCount); }

    /// Resizes and clears the arena. Every Word becomes zero, which is the encoding of
    /// logic 0 in both planes; the caller then writes each object's time-zero value,
    /// which for a four-state object is X.
    void resize(std::uint32_t wordCount) { storage.assign(wordCount, 0); }

    [[nodiscard]] std::uint32_t words() const noexcept {
        return static_cast<std::uint32_t>(storage.size());
    }

    /// Returns a mutable view of the value at `wordOffset`.
    [[nodiscard]] eir::BitRef at(std::uint32_t wordOffset, std::uint32_t bitWidth) noexcept {
        ENZAN_ASSERT(wordOffset + eir::storageWordsFor(bitWidth) <= storage.size(),
                     "arena access out of range");
        return {storage.data() + wordOffset, bitWidth};
    }

    [[nodiscard]] eir::ConstBitRef at(std::uint32_t wordOffset,
                                      std::uint32_t bitWidth) const noexcept {
        ENZAN_ASSERT(wordOffset + eir::storageWordsFor(bitWidth) <= storage.size(),
                     "arena access out of range");
        return {storage.data() + wordOffset, bitWidth};
    }

    /// The whole arena, for checkpointing and for the waveform database's bulk reads.
    [[nodiscard]] std::span<const eir::Word> all() const noexcept { return storage; }

private:
    std::vector<eir::Word> storage;
};

} // namespace enzan::runtime
