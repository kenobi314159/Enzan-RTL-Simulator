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

std::string colonSeparated(std::string_view name) {
    std::string path;
    path.reserve(name.size());
    int depth = 0;
    bool escaped = false;
    for (const char character : name) {
        if (character == '\\') escaped = !escaped;
        if (!escaped && character == '(') ++depth;
        if (!escaped && character == ')') --depth;
        path += !escaped && depth == 0 && character == '.' ? ':' : character;
    }
    return path;
}

} // namespace enzan
