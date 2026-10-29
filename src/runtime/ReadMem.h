// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::runtime {

/// A memory as `$readmemh` sees it: one unpacked dimension of elements, stored flat with the
/// left element highest.
struct MemoryShape {
    std::int64_t left = 0;
    std::int64_t right = 0;
    std::uint32_t elementWidth = 0;
};

/// Loads the text of a `$readmemh` or `$readmemb` file into `memory` (IEEE 1800 section 21.4),
/// and returns what went wrong, if anything, one line per problem.
///
/// The text is words separated by white space and comments, hexadecimal or binary as `hex`
/// says, with `x`, `z` and `_` allowed, and `@` followed by a hexadecimal address to say where
/// the next word goes. Words load from `start` towards `end`. Without a start, from the lowest
/// address to the highest, whichever way the memory is declared; with a start alone, to the
/// highest address. Loading stops at the first problem: an address or a word outside the
/// range, or a character that is no digit. An element no word reaches keeps its value.
std::vector<std::string> loadMemoryImage(std::string_view text, bool hex, const MemoryShape& shape,
                                         std::optional<std::int64_t> start,
                                         std::optional<std::int64_t> end, eir::BitRef memory);

} // namespace enzan::runtime
