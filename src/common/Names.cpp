// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Names.h"

#include <algorithm>

namespace enzan {

namespace {

char folded(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

} // namespace

bool equalsIgnoringCase(std::string_view left, std::string_view right) noexcept {
    return std::ranges::equal(left, right, [](char a, char b) { return folded(a) == folded(b); });
}

} // namespace enzan
