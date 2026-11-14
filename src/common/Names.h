// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace enzan {

/// Whether `left` and `right` are one name in any case, as VHDL compares its names and a library's.
///
/// Only ASCII letters are folded: VHDL's basic identifiers are ASCII, and folding by the C locale
/// would make the answer depend on the environment.
[[nodiscard]] bool equalsIgnoringCase(std::string_view left, std::string_view right) noexcept;

/// `names` as a diagnostic lists them, in the order given: `'a', 'b'`, or `(none)`.
template<typename Range>
[[nodiscard]] std::string quotedNames(const Range& names) {
    if (names.empty()) return "(none)";

    std::string result;
    for (const std::string_view name : names) {
        if (!result.empty()) result += ", ";
        result += '\'';
        result += name;
        result += '\'';
    }
    return result;
}

} // namespace enzan
