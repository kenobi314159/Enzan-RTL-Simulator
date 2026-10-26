// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace enzan::eir {

/// One dimension of an array, packed or unpacked, as declared: `[0:15]` is a left of 0 and a
/// right of 15.
///
/// Kept for what reads the array as an array — VPI's `vpiRegArray`, a waveform file's leaving
/// memories out, a wave window's showing one — and not for simulating it. Storage is flat: the
/// array's elements are one vector of its bits, left element highest, as a packed array's are
/// (DESIGN_GUIDE.md section 4.3), so every operation on it is an operation on bits.
///
/// In a header of its own, which needs nothing else, because the dimensions cross to the GUI and
/// a header moc reads must not reach the rest of EIR: the moc of Qt 6.4 cannot parse what
/// `common/SourceLocation.h` includes.
struct ArrayDimension {
    std::int32_t left = 0;
    std::int32_t right = 0;

    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(left > right ? left - right : right - left) + 1U;
    }

    friend bool operator==(ArrayDimension, ArrayDimension) noexcept = default;
};

} // namespace enzan::eir
