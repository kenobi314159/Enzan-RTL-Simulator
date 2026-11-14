// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace enzan {

/// Whether `left` and `right` are one name in any case, as VHDL compares its names and a library's.
///
/// Only ASCII letters are folded: VHDL's basic identifiers are ASCII, and folding by the C locale
/// would make the answer depend on the environment.
[[nodiscard]] bool equalsIgnoringCase(std::string_view left, std::string_view right) noexcept;

} // namespace enzan
