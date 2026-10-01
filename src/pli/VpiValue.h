// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include "vpi_user.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enzan::pli {

/// Where the strings and vectors handed to an application by `vpi_get_value` live.
///
/// The standard says such a result stays valid until the next call, and a strict reading would
/// allow one buffer. A ring of them is kept instead, because applications routinely hold two
/// results at once — `printf("%s -> %s", old, new)` built from two calls — and one buffer would
/// turn that into a read of freed memory in the simulator's address space.
class ValueStorage {
public:
    /// Returns a string buffer, reusing the oldest.
    std::string& nextString() {
        std::string& slot = strings[next];
        next = (next + 1) % Depth;
        return slot;
    }

    /// Returns a vector buffer, reusing the oldest.
    std::vector<s_vpi_vecval>& nextVector() {
        std::vector<s_vpi_vecval>& slot = vectors[nextVectorSlot];
        nextVectorSlot = (nextVectorSlot + 1) % Depth;
        return slot;
    }

private:
    static constexpr std::size_t Depth = 8;

    std::array<std::string, Depth> strings;
    std::array<std::vector<s_vpi_vecval>, Depth> vectors;
    std::size_t next = 0;
    std::size_t nextVectorSlot = 0;
};

/// Fills `value` with `bits`, in the format `value.format` asks for.
///
/// The conversions are IEEE 1364's: a binary, octal or hexadecimal string has one character
/// per digit of the width, with `x` or `z` for a digit wholly unknown and `X` or `Z` for one
/// partly so; a decimal string is `x` or `z` when every bit is, and `X` when only some are;
/// `vpiIntVal` reads unknown bits as 0, as the standard says. `vpiObjTypeVal` chooses
/// `vpiScalarVal` for one bit and `vpiVectorVal` otherwise, and says so in `value.format`.
///
/// Returns false having described the problem in `error` when the format is not one Enzan
/// produces — strengths and times have no meaning for a packed vector.
bool readValue(eir::ConstBitRef bits, bool isSigned, s_vpi_value& value, ValueStorage& storage,
               std::string& error);

/// Converts what an application passed to `vpi_put_value` into a vector `width` bits wide.
///
/// Strings that are shorter than the width are zero-extended and longer ones lose their most
/// significant digits, which is what assigning a sized literal does. `vpiIntVal` is a signed
/// integer and is sign-extended; `vpiRealVal` is rounded to the nearest integer, as assigning
/// a `real` to a vector is.
std::optional<eir::BitVector> parseValue(const s_vpi_value& value, std::uint32_t width,
                                         std::string& error);

} // namespace enzan::pli
