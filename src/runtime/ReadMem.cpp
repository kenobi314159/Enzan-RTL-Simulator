// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/ReadMem.h"

#include "eir/BitOps.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace enzan::runtime {

namespace {

/// The value of one digit, as its bits from the least significant: four for hexadecimal and
/// one for binary, all X for `x` and all Z for `z`. Nothing for a character that is no digit.
std::optional<std::vector<eir::Logic>> digitBits(char digit, bool hex) {
    const std::size_t count = hex ? 4 : 1;
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(digit)));
    if (lower == 'x') return std::vector<eir::Logic>(count, eir::Logic::Unknown);
    if (lower == 'z') return std::vector<eir::Logic>(count, eir::Logic::HighZ);

    unsigned value = 0;
    if (lower >= '0' && lower <= (hex ? '9' : '1')) {
        value = static_cast<unsigned>(lower - '0');
    } else if (hex && lower >= 'a' && lower <= 'f') {
        value = static_cast<unsigned>(lower - 'a' + 10);
    } else {
        return std::nullopt;
    }

    std::vector<eir::Logic> bits(count);
    for (std::size_t bit = 0; bit < count; ++bit)
        bits[bit] = ((value >> bit) & 1U) != 0 ? eir::Logic::One : eir::Logic::Zero;
    return bits;
}

} // namespace

std::vector<std::string> loadMemoryImage(std::string_view text, bool hex, const MemoryShape& shape,
                                         std::optional<std::int64_t> start,
                                         std::optional<std::int64_t> end, eir::BitRef memory) {
    std::vector<std::string> problems;
    const std::int64_t lowest = std::min(shape.left, shape.right);
    const std::int64_t highest = std::max(shape.left, shape.right);
    const auto count = static_cast<std::uint64_t>(highest - lowest + 1);

    const std::int64_t first = start.value_or(lowest);
    const std::int64_t last = end.value_or(highest);
    for (const std::int64_t address : {first, last}) {
        if (address < lowest || address > highest) {
            problems.push_back(std::format("address {} is outside the memory's [{}:{}]", address,
                                           shape.left, shape.right));
            return problems;
        }
    }
    const std::int64_t step = first <= last ? 1 : -1;
    const std::int64_t rangeLow = std::min(first, last);
    const std::int64_t rangeHigh = std::max(first, last);

    std::int64_t address = first;
    std::uint64_t words = 0;
    bool addressed = false;
    std::size_t line = 1;
    std::size_t position = 0;

    // Stores one word at `address`, with the left element highest.
    const auto store = [&](const std::vector<eir::Logic>& bits) {
        const auto element = static_cast<std::uint64_t>(
            address >= shape.left ? address - shape.left : shape.left - address);
        const std::uint64_t offset = (count - 1 - element) * shape.elementWidth;
        eir::BitVector word = eir::BitVector::filled(shape.elementWidth, eir::Logic::Zero);
        for (std::size_t bit = 0; bit < bits.size() && bit < shape.elementWidth; ++bit)
            word.mutableRef().set(static_cast<std::uint32_t>(bit), bits[bit]);
        eir::bitops::assignPartAt(memory, word.ref(), static_cast<std::uint32_t>(offset));
    };

    while (position < text.size()) {
        const char c = text[position];
        if (c == '\n') {
            ++line;
            ++position;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++position;
            continue;
        }
        if (text.substr(position, 2) == "//") {
            position = text.find('\n', position);
            if (position == std::string_view::npos) break;
            continue;
        }
        if (text.substr(position, 2) == "/*") {
            const std::size_t close = text.find("*/", position + 2);
            const std::size_t stop = close == std::string_view::npos ? text.size() : close + 2;
            line += static_cast<std::size_t>(
                std::count(text.begin() + static_cast<std::ptrdiff_t>(position),
                           text.begin() + static_cast<std::ptrdiff_t>(stop), '\n'));
            position = stop;
            continue;
        }

        // One token: an address or a word, up to the next separator.
        const bool isAddress = c == '@';
        std::size_t stop = position + (isAddress ? 1 : 0);
        while (stop < text.size() && std::isspace(static_cast<unsigned char>(text[stop])) == 0
               && text.substr(stop, 2) != "//" && text.substr(stop, 2) != "/*")
            ++stop;
        const std::string_view token =
            text.substr(position + (isAddress ? 1 : 0), stop - position - (isAddress ? 1 : 0));
        position = stop;

        // Digits from the least significant end, so a short word is zero-extended.
        std::vector<eir::Logic> bits;
        for (auto digit = token.rbegin(); digit != token.rend(); ++digit) {
            if (*digit == '_') continue;
            const std::optional<std::vector<eir::Logic>> value =
                digitBits(*digit, hex || isAddress);
            if (!value.has_value()) {
                problems.push_back(std::format("'{}' on line {} is not a {} number", token, line,
                                               hex || isAddress ? "hexadecimal" : "binary"));
                return problems;
            }
            bits.insert(bits.end(), value->begin(), value->end());
        }
        if (bits.empty()) {
            problems.push_back(
                std::format("line {} has an empty {}", line, isAddress ? "address" : "word"));
            return problems;
        }

        if (isAddress) {
            std::int64_t value = 0;
            for (std::size_t bit = bits.size(); bit-- > 0;) {
                if (bits[bit] != eir::Logic::Zero && bits[bit] != eir::Logic::One) {
                    problems.push_back(std::format("the address on line {} is not known", line));
                    return problems;
                }
                if (bit < 63) value = (value << 1) | (bits[bit] == eir::Logic::One ? 1 : 0);
            }
            if (value < rangeLow || value > rangeHigh) {
                problems.push_back(std::format("address {} on line {} is outside [{}:{}]", value,
                                               line, first, last));
                return problems;
            }
            address = value;
            addressed = true;
            continue;
        }

        if (address < rangeLow || address > rangeHigh) {
            problems.push_back(
                std::format("line {} has more words than [{}:{}] holds", line, first, last));
            return problems;
        }
        store(bits);
        ++words;
        address += step;
    }

    if (!addressed && std::cmp_less(words, rangeHigh - rangeLow + 1)) {
        problems.push_back(std::format("{} words fill only part of [{}:{}]", words, first, last));
    }
    return problems;
}

} // namespace enzan::runtime
