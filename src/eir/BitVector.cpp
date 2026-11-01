// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/BitVector.h"

#include "common/Assert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <utility>

namespace enzan::eir {

namespace {

/// Divides the little-endian magnitude `value` by `divisor` in place and returns
/// the remainder.
///
/// Used only for decimal rendering, which is off the simulation hot path; the
/// simple long-division loop is preferred over anything cleverer because a
/// mis-rendered value in a transcript is indistinguishable from a wrong
/// simulation result to the user reading it.
std::uint64_t divModSmall(std::span<Word> value, std::uint32_t divisor) noexcept {
    ENZAN_ASSERT(divisor > 0, "division by zero");

    std::uint64_t remainder = 0;
    for (std::size_t index = value.size(); index-- > 0;) {
        // Split the 64-bit limb into halves so that the intermediate dividend
        // fits in 64 bits without needing a 128-bit type.
        const std::uint64_t high = (remainder << 32) | (value[index] >> 32);
        const std::uint64_t highQuotient = high / divisor;
        remainder = high % divisor;

        const std::uint64_t low = (remainder << 32) | (value[index] & 0xFFFF'FFFFU);
        const std::uint64_t lowQuotient = low / divisor;
        remainder = low % divisor;

        value[index] = (highQuotient << 32) | lowQuotient;
    }
    return remainder;
}

/// Computes `value = value * multiplier + addend` in place, discarding any carry
/// out of the top limb. Used for decimal literal parsing; overflow past the
/// declared width is the truncation IEEE 1800 specifies for an oversized literal.
void mulAddSmall(std::span<Word> value, std::uint64_t multiplier, std::uint64_t addend) noexcept {
    std::uint64_t carry = addend;
    for (Word& limb : value) {
        // 32-bit halves again, for the same reason as above.
        const std::uint64_t low = ((limb & 0xFFFF'FFFFU) * multiplier) + (carry & 0xFFFF'FFFFU);
        const std::uint64_t high = ((limb >> 32) * multiplier) + (low >> 32) + (carry >> 32);
        limb = (high << 32) | (low & 0xFFFF'FFFFU);
        carry = high >> 32;
    }
}

/// True when every limb is zero.
bool allZero(std::span<const Word> words) noexcept {
    return std::ranges::all_of(words, [](Word word) { return word == 0; });
}

/// Returns the number of bits one digit of `radix` covers, or 0 for decimal.
constexpr std::uint32_t bitsPerDigit(Radix radix) noexcept {
    switch (radix) {
    case Radix::Binary: return 1;
    case Radix::Octal: return 3;
    case Radix::Hexadecimal: return 4;
    case Radix::Decimal: return 0;
    case Radix::Ascii: return 8;
    }
    return 0;
}

/// The bytes of `value` as characters; see toString.
std::string asciiString(ConstBitRef value) {
    std::string result;
    const std::uint32_t bytes = (value.width() + 7) / 8;
    for (std::uint32_t index = bytes; index-- > 0;) {
        unsigned character = 0;
        bool unknown = false;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t position = (index * 8) + bit;
            if (position >= value.width()) break;
            const Logic logic = value.get(position);
            if (logic == Logic::One) character |= 1U << bit;
            if (logic == Logic::Unknown || logic == Logic::HighZ) unknown = true;
        }

        if (unknown) {
            result += "\xEF\xBF\xBD";
            continue;
        }
        if (character == 0 && result.empty()) continue;

        switch (character) {
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        case '\\': result += "\\\\"; break;
        case 0: result += "\\0"; break;
        default:
            if (character >= 0x20 && character < 0x7F) {
                result.push_back(static_cast<char>(character));
            } else {
                result += std::format("\\x{:02x}", character);
            }
            break;
        }
    }
    return result;
}

/// Maps a digit character to its value in `base`, or 255 when it is not a digit
/// of that base. `x` and `z` are handled by the caller, not here.
std::uint8_t digitValue(char character, std::uint32_t base) noexcept {
    std::uint32_t value = 0;
    if (character >= '0' && character <= '9')
        value = static_cast<std::uint32_t>(character - '0');
    else if (character >= 'a' && character <= 'f')
        value = static_cast<std::uint32_t>(character - 'a') + 10;
    else if (character >= 'A' && character <= 'F')
        value = static_cast<std::uint32_t>(character - 'A') + 10;
    else
        return 255;

    return value < base ? static_cast<std::uint8_t>(value) : static_cast<std::uint8_t>(255);
}

} // namespace

// ---------------------------------------------------------------------------
// ConstBitRef
// ---------------------------------------------------------------------------

Logic ConstBitRef::get(std::uint32_t index) const noexcept {
    ENZAN_ASSERT(index < bitWidth, "bit index out of range");

    const std::uint32_t word = index / BitsPerWord;
    const std::uint32_t bit = index % BitsPerWord;
    const auto a = static_cast<std::uint8_t>((aval()[word] >> bit) & 1U);
    const auto b = static_cast<std::uint8_t>((bval()[word] >> bit) & 1U);
    return static_cast<Logic>(a | static_cast<std::uint8_t>(b << 1U));
}

bool ConstBitRef::isTwoState() const noexcept {
    return allZero(bvalWords());
}

bool ConstBitRef::isAll(Logic value) const noexcept {
    const bool wantA = (static_cast<std::uint8_t>(value) & 1U) != 0;
    const bool wantB = (static_cast<std::uint8_t>(value) & 2U) != 0;
    const Word mask = topWordMask(bitWidth);
    const std::uint32_t count = words();

    for (std::uint32_t index = 0; index < count; ++index) {
        const Word limbMask = index + 1 == count ? mask : ~Word{0};
        const Word expectedA = wantA ? limbMask : Word{0};
        const Word expectedB = wantB ? limbMask : Word{0};
        if ((aval()[index] & limbMask) != expectedA || (bval()[index] & limbMask) != expectedB)
            return false;
    }
    return true;
}

bool ConstBitRef::isZero() const noexcept {
    return allZero(avalWords()) && allZero(bvalWords());
}

std::uint64_t ConstBitRef::toUInt64() const noexcept {
    return aval()[0] & (bitWidth >= BitsPerWord ? ~Word{0} : topWordMask(bitWidth));
}

std::int64_t ConstBitRef::toInt64() const noexcept {
    const std::uint64_t magnitude = toUInt64();
    if (bitWidth >= BitsPerWord) return static_cast<std::int64_t>(magnitude);

    // Sign-extend from the declared width. Written as a shift pair rather than a
    // conditional negate so that the result is well defined for every width.
    const std::uint32_t shift = BitsPerWord - bitWidth;
    return static_cast<std::int64_t>(magnitude << shift) >> shift;
}

bool ConstBitRef::identicalTo(ConstBitRef other) const noexcept {
    ENZAN_ASSERT(bitWidth == other.bitWidth, "identicalTo requires equal widths");

    return std::ranges::equal(avalWords(), other.avalWords())
           && std::ranges::equal(bvalWords(), other.bvalWords());
}

// ---------------------------------------------------------------------------
// BitRef
// ---------------------------------------------------------------------------

void BitRef::set(std::uint32_t index, Logic value) const noexcept {
    ENZAN_ASSERT(index < bitWidth, "bit index out of range");

    const std::uint32_t word = index / BitsPerWord;
    const std::uint32_t bit = index % BitsPerWord;
    const Word mask = Word{1} << bit;
    const auto raw = static_cast<std::uint8_t>(value);

    aval()[word] = (aval()[word] & ~mask) | ((raw & 1U) != 0 ? mask : Word{0});
    bval()[word] = (bval()[word] & ~mask) | ((raw & 2U) != 0 ? mask : Word{0});
}

void BitRef::fill(Logic value) const noexcept {
    const auto raw = static_cast<std::uint8_t>(value);
    const Word a = (raw & 1U) != 0 ? ~Word{0} : Word{0};
    const Word b = (raw & 2U) != 0 ? ~Word{0} : Word{0};

    std::ranges::fill(avalWords(), a);
    std::ranges::fill(bvalWords(), b);
    canonicalize();
}

void BitRef::canonicalize() const noexcept {
    const Word mask = topWordMask(bitWidth);
    const std::uint32_t top = words() - 1;
    aval()[top] &= mask;
    bval()[top] &= mask;
}

// ---------------------------------------------------------------------------
// BitVector
// ---------------------------------------------------------------------------

BitVector::BitVector(std::uint32_t bits) : bitWidth(bits) {
    ENZAN_ALWAYS_ASSERT(bits > 0, "a bit vector must have at least one bit");
    storage.assign(storageWordsFor(bits), 0);
    mutableRef().fill(Logic::Unknown);
}

BitVector BitVector::filled(std::uint32_t bits, Logic value) {
    BitVector result(bits);
    result.mutableRef().fill(value);
    return result;
}

BitVector BitVector::fromUInt64(std::uint64_t value, std::uint32_t bits) {
    BitVector result = filled(bits, Logic::Zero);
    result.storage[0] = value;
    result.mutableRef().canonicalize();
    return result;
}

BitVector BitVector::fromInt64(std::int64_t value, std::uint32_t bits) {
    const bool negative = value < 0;
    BitVector result = filled(bits, negative ? Logic::One : Logic::Zero);
    result.storage[0] = static_cast<std::uint64_t>(value);

    // Limbs above the first were filled with the sign already; only the first
    // needs the actual bits, and the top limb needs re-masking.
    result.mutableRef().canonicalize();
    return result;
}

BitVector BitVector::from(ConstBitRef source) {
    BitVector result(source.width());
    std::ranges::copy(source.avalWords(), result.mutableRef().aval());
    std::ranges::copy(source.bvalWords(), result.mutableRef().bval());
    return result;
}

BitVector BitVector::resized(std::uint32_t newWidth, bool signExtend) const {
    ENZAN_ALWAYS_ASSERT(newWidth > 0, "a bit vector must have at least one bit");

    // The fill value for the new high bits: the source's top bit when
    // sign-extending, zero otherwise. Extending an X sign bit yields X, which is
    // the pessimistic and correct answer.
    Logic fillValue = Logic::Zero;
    if (signExtend && newWidth > bitWidth) fillValue = get(bitWidth - 1);

    BitVector result = filled(newWidth, fillValue);
    const std::uint32_t copied = std::min(newWidth, bitWidth);
    for (std::uint32_t index = 0; index < copied; ++index) result.set(index, get(index));

    return result;
}

bool operator==(const BitVector& left, const BitVector& right) noexcept {
    return left.bitWidth == right.bitWidth && left.ref().identicalTo(right.ref());
}

std::optional<BitVector> BitVector::parse(std::string_view text) {
    if (text.empty()) return std::nullopt;

    const std::size_t tick = text.find('\'');

    // No base tick: a plain decimal literal, 32 bits and signed per IEEE 1800.
    if (tick == std::string_view::npos) {
        BitVector result = filled(32, Logic::Zero);
        bool sawDigit = false;
        for (const char character : text) {
            if (character == '_') continue;
            const std::uint8_t digit = digitValue(character, 10);
            if (digit == 255) return std::nullopt;
            mulAddSmall(std::span(result.storage).subspan(0, wordsFor(32)), 10, digit);
            sawDigit = true;
        }
        if (!sawDigit) return std::nullopt;
        result.mutableRef().canonicalize();
        return result;
    }

    // Size, if given.
    std::uint32_t width = 0;
    bool sized = false;
    if (tick > 0) {
        const std::string_view sizeText = text.substr(0, tick);
        const auto* first = sizeText.data();
        const auto* last = first + sizeText.size();
        const std::from_chars_result parsed = std::from_chars(first, last, width);
        if (parsed.ec != std::errc() || parsed.ptr != last || width == 0) return std::nullopt;
        sized = true;
    }

    std::string_view rest = text.substr(tick + 1);
    if (rest.empty()) return std::nullopt;

    // Unsized fill literals: '0 '1 'x 'z. These have no width of their own, so a
    // single bit is returned and the caller extends it in context.
    if (!sized && rest.size() == 1) {
        switch (rest[0]) {
        case '0': return filled(1, Logic::Zero);
        case '1': return filled(1, Logic::One);
        case 'x':
        case 'X': return filled(1, Logic::Unknown);
        case 'z':
        case 'Z': return filled(1, Logic::HighZ);
        default: return std::nullopt;
        }
    }

    // An `s` before the base marks the literal signed. Signedness is a property
    // of the type rather than of the storage, so it is accepted and dropped here;
    // the frontend records it on the expression's type.
    if (rest[0] == 's' || rest[0] == 'S') rest.remove_prefix(1);
    if (rest.empty()) return std::nullopt;

    std::uint32_t base = 0;
    switch (rest[0]) {
    case 'b':
    case 'B': base = 2; break;
    case 'o':
    case 'O': base = 8; break;
    case 'd':
    case 'D': base = 10; break;
    case 'h':
    case 'H': base = 16; break;
    default: return std::nullopt;
    }
    rest.remove_prefix(1);

    if (!sized) width = 32;

    std::string digits;
    digits.reserve(rest.size());
    for (const char character : rest) {
        if (character != '_') digits.push_back(character);
    }
    if (digits.empty()) return std::nullopt;

    BitVector result = filled(width, Logic::Zero);

    if (base == 10) {
        // Decimal cannot express per-digit unknowns, so IEEE 1800 allows a decimal
        // literal to be exactly one `x` or one `z` and nothing else.
        if (digits.size() == 1 && (digits[0] == 'x' || digits[0] == 'X'))
            return filled(width, Logic::Unknown);
        if (digits.size() == 1 && (digits[0] == 'z' || digits[0] == 'Z'))
            return filled(width, Logic::HighZ);

        for (const char character : digits) {
            const std::uint8_t digit = digitValue(character, 10);
            if (digit == 255) return std::nullopt;
            mulAddSmall(std::span(result.storage).subspan(0, wordsFor(width)), 10, digit);
        }
        result.mutableRef().canonicalize();
        return result;
    }

    // Power-of-two bases fill from the least significant digit upwards, so that a
    // literal with more digits than the declared width truncates at the top as
    // IEEE 1800 requires.
    std::uint32_t perDigit = 4;
    if (base == 2)
        perDigit = 1;
    else if (base == 8)
        perDigit = 3;
    std::uint32_t bitIndex = 0;

    // What the bits above the digits hold. IEEE 1800 section 5.7.1 pads with zeros, unless the
    // leftmost digit is an x or a z, which pads with x or z: `8'hx` is eight unknown bits.
    Logic leftFill = Logic::Zero;

    for (std::size_t position = digits.size(); position-- > 0;) {
        const char character = digits[position];
        Logic unknownFill = Logic::Zero;
        bool isUnknownDigit = false;

        if (character == 'x' || character == 'X') {
            unknownFill = Logic::Unknown;
            isUnknownDigit = true;
        } else if (character == 'z' || character == 'Z' || character == '?') {
            unknownFill = Logic::HighZ;
            isUnknownDigit = true;
        }
        if (position == 0) leftFill = unknownFill;

        std::uint8_t digit = 0;
        if (!isUnknownDigit) {
            digit = digitValue(character, base);
            if (digit == 255) return std::nullopt;
        }

        for (std::uint32_t offset = 0; offset < perDigit; ++offset) {
            if (bitIndex >= width) {
                ++bitIndex;
                continue;
            }
            if (isUnknownDigit) {
                result.set(bitIndex, unknownFill);
            } else {
                result.set(bitIndex, ((digit >> offset) & 1U) != 0 ? Logic::One : Logic::Zero);
            }
            ++bitIndex;
        }
    }

    if (leftFill != Logic::Zero) {
        for (; bitIndex < width; ++bitIndex) result.set(bitIndex, leftFill);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::string toString(ConstBitRef value, Radix radix, bool isSigned) {
    if (radix == Radix::Ascii) return asciiString(value);

    if (radix != Radix::Decimal) {
        const std::uint32_t perDigit = bitsPerDigit(radix);
        const std::uint32_t digitCount = (value.width() + perDigit - 1) / perDigit;

        std::string result;
        result.reserve(digitCount);

        for (std::size_t digit = digitCount; digit-- > 0;) {
            const std::uint32_t base = static_cast<std::uint32_t>(digit) * perDigit;
            std::uint32_t unknownCount = 0;
            std::uint32_t highZCount = 0;
            std::uint32_t present = 0;
            std::uint32_t bits = 0;

            for (std::uint32_t offset = 0; offset < perDigit; ++offset) {
                const std::uint32_t index = base + offset;
                if (index >= value.width()) break;
                ++present;

                switch (value.get(index)) {
                case Logic::Zero: break;
                case Logic::One: bits |= 1U << offset; break;
                case Logic::Unknown: ++unknownCount; break;
                case Logic::HighZ: ++highZCount; break;
                }
            }

            // A digit is lowercase only when every bit it covers carries the same
            // unknown value; a partially unknown digit is uppercase, so that a
            // reader can tell "entirely unknown" from "contaminated".
            if (unknownCount == present)
                result.push_back('x');
            else if (highZCount == present)
                result.push_back('z');
            else if (unknownCount > 0)
                result.push_back('X');
            else if (highZCount > 0)
                result.push_back('Z');
            else
                result.push_back("0123456789abcdef"[bits]);
        }

        return result;
    }

    if (value.isAll(Logic::Unknown)) return "x";
    if (value.isAll(Logic::HighZ)) return "z";
    if (value.hasUnknown()) {
        // X when any bit is X, and Z when the unknown bits are all Z (IEEE 1800 section
        // 21.2.1.3). X is (1,1) in the two planes.
        for (std::uint32_t index = 0; index < value.words(); ++index) {
            if ((value.aval()[index] & value.bval()[index]) != 0) return "X";
        }
        return "Z";
    }

    // Two's-complement negation before rendering, so that a signed value prints
    // its magnitude behind a sign rather than as a huge positive number.
    BitVector magnitude = BitVector::from(value);
    std::string sign;
    if (isSigned && value.get(value.width() - 1) == Logic::One) {
        sign = "-";
        const std::uint32_t count = magnitude.width();
        BitVector inverted = BitVector::filled(count, Logic::Zero);
        for (std::uint32_t index = 0; index < count; ++index) {
            inverted.set(index, magnitude.get(index) == Logic::One ? Logic::Zero : Logic::One);
        }
        magnitude = std::move(inverted);
        mulAddSmall(magnitude.mutableRef().avalWords(), 1, 1);
        magnitude.mutableRef().canonicalize();
    }

    std::string digits;
    const std::span<Word> limbs = magnitude.mutableRef().avalWords();
    do {
        const std::uint64_t remainder = divModSmall(limbs, 10);
        digits.push_back(static_cast<char>('0' + remainder));
    } while (!allZero(limbs));

    std::ranges::reverse(digits);
    return sign + digits;
}

} // namespace enzan::eir
