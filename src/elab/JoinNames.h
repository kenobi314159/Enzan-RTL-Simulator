// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace enzan::elab {

/// Joins a list of names for a diagnostic, in the order given.
template<typename Range>
std::string joinNames(const Range& names) {
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

} // namespace enzan::elab
