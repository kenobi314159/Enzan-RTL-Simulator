// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/ValueText.h"

#include "eir/BitOps.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <vector>

namespace enzan::gui {

namespace {

ValueReading accepted(eir::BitVector value, std::vector<PartialDigit> partial = {}) {
    return {.value = std::move(value), .partial = std::move(partial), .problem = {}};
}

ValueReading refused(QString problem) {
    return {.value = std::nullopt, .problem = std::move(problem)};
}

QString bitsText(std::uint32_t width) {
    return width == 1 ? QStringLiteral("1 bit") : QStringLiteral("%1 bits").arg(width);
}

QString tooWide(QStringView text, std::uint32_t width) {
    return QStringLiteral("'%1' does not fit in %2").arg(text, bitsText(width));
}

QString radixName(eir::Radix radix) {
    switch (radix) {
    case eir::Radix::Binary: return QStringLiteral("binary");
    case eir::Radix::Octal: return QStringLiteral("octal");
    case eir::Radix::Decimal: return QStringLiteral("decimal");
    case eir::Radix::Hexadecimal: return QStringLiteral("hexadecimal");
    case eir::Radix::Ascii: return QStringLiteral("ASCII");
    }
    return {};
}

/// The bits one digit of `radix` sets: an upper bound for decimal, whose digits are worth a
/// little over three bits each.
std::uint32_t bitsPerDigit(eir::Radix radix) {
    switch (radix) {
    case eir::Radix::Binary: return 1;
    case eir::Radix::Octal: return 3;
    default: return 4;
    }
}

char baseLetter(eir::Radix radix) {
    switch (radix) {
    case eir::Radix::Binary: return 'b';
    case eir::Radix::Octal: return 'o';
    case eir::Radix::Decimal: return 'd';
    default: return 'h';
    }
}

/// The radix a literal's base letter names, or nothing.
std::optional<eir::Radix> radixOfBase(char base) {
    switch (base) {
    case 'b':
    case 'B': return eir::Radix::Binary;
    case 'o':
    case 'O': return eir::Radix::Octal;
    case 'd':
    case 'D': return eir::Radix::Decimal;
    case 'h':
    case 'H': return eir::Radix::Hexadecimal;
    default: return std::nullopt;
    }
}

/// `value` at `width` bits, extended with zeros, or nothing when a bit above `width` is not zero.
std::optional<eir::BitVector> fitted(const eir::BitVector& value, std::uint32_t width) {
    for (std::uint32_t bit = width; bit < value.width(); ++bit) {
        if (value.get(bit) != eir::Logic::Zero) return std::nullopt;
    }
    return value.resized(width, false);
}

/// True for a digit of `radix`, x and z among them where a digit can hold one. A literal takes
/// `?` for z as well, as SystemVerilog does; the value column never writes one.
bool isDigitOf(char16_t code, eir::Radix radix, bool literal) {
    const bool unknown =
        code == u'x' || code == u'X' || code == u'z' || code == u'Z' || (literal && code == u'?');
    switch (radix) {
    case eir::Radix::Binary: return unknown || code == u'0' || code == u'1';
    case eir::Radix::Octal: return unknown || (code >= u'0' && code <= u'7');
    case eir::Radix::Decimal: return code >= u'0' && code <= u'9';
    case eir::Radix::Hexadecimal:
        return unknown || (code >= u'0' && code <= u'9') || (code >= u'a' && code <= u'f')
               || (code >= u'A' && code <= u'F');
    case eir::Radix::Ascii: return false;
    }
    return false;
}

/// Reads digits written in `radix` into `width` bits, as `<width>'<radix><digits>` would be read
/// with the padding of IEEE 1800, or nothing when they are not digits of it.
///
/// The digits may number as many as the width needs and leading zeros beyond. The top one of them
/// reaches past a width its bits do not divide, as the value column writes it there, and an x or
/// a z covers as much of it as there is; a one past the width, or any other digit beyond, is a
/// value too wide.
///
/// Outside a literal, an `X` or a `Z` is a partial digit wherever it covers more than one bit, as
/// the column writes one.
std::optional<ValueReading> readDigits(QStringView text, std::uint32_t width, eir::Radix radix,
                                       bool literal) {
    QString digits;
    digits.reserve(text.size());
    for (const QChar character : text) {
        if (character != u'_') digits.push_back(character);
    }
    if (digits.isEmpty()) return std::nullopt;

    // The kind of partial digit `code` is over bits [high, low], if it is one.
    const auto partialOf = [literal](char16_t code, std::uint32_t high,
                                     std::uint32_t low) -> std::optional<PartialDigit> {
        if (literal || high == low) return std::nullopt;
        if (code == u'X')
            return PartialDigit{.highBit = high, .lowBit = low, .kind = eir::Logic::Unknown};
        if (code == u'Z')
            return PartialDigit{.highBit = high, .lowBit = low, .kind = eir::Logic::HighZ};
        return std::nullopt;
    };

    // Decimal cannot place an unknown in a digit, so its x and z stand for the whole value, as
    // the column writes them.
    if (radix == eir::Radix::Decimal && digits.size() == 1) {
        const char16_t code = digits.front().unicode();
        std::vector<PartialDigit> partial;
        if (const std::optional<PartialDigit> digit = partialOf(code, width - 1, 0))
            partial.push_back(*digit);
        if (code == u'x' || code == u'X')
            return accepted(eir::BitVector::filled(width, eir::Logic::Unknown), std::move(partial));
        if (code == u'z' || code == u'Z')
            return accepted(eir::BitVector::filled(width, eir::Logic::HighZ), std::move(partial));
    }

    if (!std::ranges::all_of(digits, [radix, literal](QChar character) {
            return isDigitOf(character.unicode(), radix, literal);
        }))
        return std::nullopt;

    const std::string base(1, baseLetter(radix));
    if (radix == eir::Radix::Decimal) {
        // As many bits as the digits can reach, so that a value beyond the width is refused
        // rather than cut off.
        const auto size =
            std::max(width, static_cast<std::uint32_t>(digits.size()) * bitsPerDigit(radix));
        const std::optional<eir::BitVector> parsed =
            eir::BitVector::parse(std::to_string(size) + '\'' + base + digits.toStdString());
        if (!parsed.has_value()) return std::nullopt;

        std::optional<eir::BitVector> value = fitted(*parsed, width);
        if (!value.has_value()) return refused(tooWide(text, width));
        return accepted(std::move(*value));
    }

    const std::uint32_t perDigit = bitsPerDigit(radix);
    const std::uint32_t needed = (width + perDigit - 1) / perDigit;

    qsizetype first = 0;
    while (digits.size() - first > static_cast<qsizetype>(needed) && digits[first] == u'0') {
        ++first;
    }
    if (digits.size() - first > static_cast<qsizetype>(needed))
        return refused(tooWide(text, width));

    const QString kept = digits.sliced(first);
    const std::uint32_t size = needed * perDigit;
    const std::optional<eir::BitVector> parsed =
        eir::BitVector::parse(std::to_string(size) + '\'' + base + kept.toStdString());
    if (!parsed.has_value()) return std::nullopt;

    for (std::uint32_t bit = width; bit < size; ++bit) {
        if (parsed->get(bit) == eir::Logic::One) return refused(tooWide(text, width));
    }

    // The leftmost first, and none past the width, which a digit always reaches into.
    std::vector<PartialDigit> partial;
    for (qsizetype position = 0; position < kept.size(); ++position) {
        const auto low = static_cast<std::uint32_t>(kept.size() - 1 - position) * perDigit;
        const std::uint32_t high = std::min(low + perDigit - 1, width - 1);
        if (const std::optional<PartialDigit> digit =
                partialOf(kept[position].unicode(), high, low))
            partial.push_back(*digit);
    }
    return accepted(parsed->resized(width, false), std::move(partial));
}

/// Reads a SystemVerilog literal into `width` bits, or nothing when `text` is no literal.
std::optional<ValueReading> readLiteral(QStringView text, std::uint32_t width) {
    // A fill literal fills its context, as it does in SystemVerilog.
    if (text.size() == 2 && text.front() == u'\'') {
        const std::optional<eir::BitVector> fill =
            eir::BitVector::parse(text.toString().toStdString());
        if (!fill.has_value()) return std::nullopt;
        return accepted(eir::BitVector::filled(width, fill->get(0)));
    }

    // An unsized literal has the width its context gives it, here the row's, and its digits are
    // read as the row's own would be: `'h1ff` is too wide for eight bits rather than `8'hff`.
    if (text.front() == u'\'') {
        QStringView rest = text.sliced(1);
        if (!rest.isEmpty() && (rest.front() == u's' || rest.front() == u'S'))
            rest = rest.sliced(1);
        if (rest.isEmpty()) return std::nullopt;

        if (rest.front().unicode() > 0x7F) return std::nullopt;
        const std::optional<eir::Radix> radix =
            radixOfBase(static_cast<char>(rest.front().unicode()));
        if (!radix.has_value()) return std::nullopt;

        std::optional<ValueReading> reading = readDigits(rest.sliced(1), width, *radix, true);
        if (reading.has_value() && !reading->value.has_value())
            reading->problem = tooWide(text, width);
        return reading;
    }

    // A sized literal is the value it says at the width it says, which is refused where that is
    // wider than the row holds and extended with zeros where it is narrower.
    //
    // Not at every width it can say, though: a typo can say billions of bits, which are made
    // whole before any is compared. Past the row, and past the four bits a character its digits
    // reach at most, a literal holds only the padding of its leftmost digit, so it is read at the
    // first width past both, which keeps a bit of that padding above the row to refuse an x by.
    std::string literal = text.toString().toStdString();
    const std::size_t tick = literal.find('\'');
    std::uint64_t stated = 0;
    const auto [end, error] = std::from_chars(literal.data(), literal.data() + tick, stated);
    const std::uint64_t enough = std::max<std::uint64_t>(width, 4 * literal.size()) + 1;
    if (error == std::errc() && end == literal.data() + tick && stated > enough)
        literal = std::to_string(enough) + literal.substr(tick);

    const std::optional<eir::BitVector> parsed = eir::BitVector::parse(literal);
    if (!parsed.has_value()) return std::nullopt;

    std::optional<eir::BitVector> value = fitted(*parsed, width);
    if (!value.has_value()) return refused(tooWide(text, width));
    return accepted(std::move(*value));
}

/// The value of one hexadecimal digit, or -1.
int hexDigit(QChar character) {
    const char16_t code = character.toLower().unicode();
    if (code >= u'0' && code <= u'9') return code - u'0';
    if (code >= u'a' && code <= u'f') return code - u'a' + 10;
    return -1;
}

/// Reads text as the value column writes it in ASCII: its characters, the escapes it writes for
/// those it cannot print, and NUL bytes on the left.
ValueReading readCharacters(QStringView text, std::uint32_t width) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(text.size()));

    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text[index];

        if (character.unicode() == 0xFFFD) {
            return refused(QStringLiteral("'%1' has �, which is a byte with an unknown bit rather "
                                          "than a character; a literal such as 8'hx names one")
                               .arg(text));
        }

        if (character != u'\\') {
            if (character.unicode() >= 0x80)
                return refused(QStringLiteral("'%1' is not ASCII").arg(character));
            bytes.push_back(static_cast<std::uint8_t>(character.unicode()));
            continue;
        }

        const QChar escaped = index + 1 < text.size() ? text[index + 1] : QChar();
        index += 1;
        if (escaped == u'n') {
            bytes.push_back('\n');
        } else if (escaped == u't') {
            bytes.push_back('\t');
        } else if (escaped == u'\\') {
            bytes.push_back('\\');
        } else if (escaped == u'0') {
            bytes.push_back(0);
        } else if (escaped == u'x' && index + 2 < text.size() && hexDigit(text[index + 1]) >= 0
                   && hexDigit(text[index + 2]) >= 0) {
            bytes.push_back(static_cast<std::uint8_t>((hexDigit(text[index + 1]) * 16)
                                                      + hexDigit(text[index + 2])));
            index += 2;
        } else {
            return refused(QStringLiteral("'%1' has an escape the value column never writes: it "
                                          "writes \\n, \\t, \\\\, \\0 and \\x with two digits")
                               .arg(text));
        }
    }

    // The last character is the least significant byte, as in a string literal.
    const auto needed = static_cast<std::uint32_t>(bytes.size() * 8);
    eir::BitVector value = eir::BitVector::filled(std::max({width, needed, 1U}), eir::Logic::Zero);
    std::uint32_t low = 0;
    for (auto byte = bytes.rbegin(); byte != bytes.rend(); ++byte, low += 8) {
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            if (((*byte >> bit) & 1U) != 0) value.set(low + bit, eir::Logic::One);
        }
    }

    std::optional<eir::BitVector> result = fitted(value, width);
    if (!result.has_value()) return refused(tooWide(text, width));
    return accepted(std::move(*result));
}

} // namespace

ValueReading readNumber(QStringView text, std::uint32_t width, eir::Radix radix) {
    const QStringView trimmed = text.trimmed();

    // Space is a character in ASCII, so only a text with nothing in it at all is empty there.
    if (text.isEmpty() || (radix != eir::Radix::Ascii && trimmed.isEmpty()))
        return refused(QStringLiteral("No value was typed"));
    if (width == 0) return refused(QStringLiteral("There are no bits to hold a value"));

    if (trimmed.contains(u'\'')) {
        if (std::optional<ValueReading> literal = readLiteral(trimmed, width)) return *literal;

        // An apostrophe is a character too, and in an ASCII row what is no literal is text.
        if (radix != eir::Radix::Ascii)
            return refused(QStringLiteral("'%1' is not a value Enzan understands").arg(trimmed));
    }

    if (radix == eir::Radix::Ascii) return readCharacters(text, width);
    if (std::optional<ValueReading> digits = readDigits(trimmed, width, radix, false))
        return *digits;
    return refused(QStringLiteral("'%1' is not a %2 number").arg(trimmed, radixName(radix)));
}

ValueReading readArray(QStringView text, std::uint32_t width,
                       std::span<const eir::ArrayDimension> dimensions, eir::Radix radix) {
    // The shapes `renderArray` writes as a list, and no others: an array whose elements are
    // single bits reads as one number.
    if (!dimensions.empty() && width % dimensions.front().size() == 0
        && text.trimmed().startsWith(u'{')) {
        const std::uint32_t count = dimensions.front().size();
        const std::uint32_t elementWidth = width / count;

        if (elementWidth > 1) {
            const std::optional<QStringList> words = splitList(text);
            if (!words.has_value())
                return refused(QStringLiteral("'%1' is not a list: its braces do not pair up")
                                   .arg(text.trimmed()));
            if (words->size() != static_cast<qsizetype>(count)) {
                return refused(QStringLiteral("'%1' lists %2, and the row has %3 elements")
                                   .arg(text.trimmed())
                                   .arg(words->size())
                                   .arg(count));
            }

            std::vector<ValueReading> parts;
            parts.reserve(count);
            for (const QString& word : *words) {
                ValueReading element = readArray(word, elementWidth, dimensions.subspan(1), radix);
                if (!element.value.has_value()) return element;
                parts.push_back(std::move(element));
            }
            return joinReadings(parts);
        }
    }

    return readNumber(text, width, radix);
}

std::optional<QStringList> splitList(QStringView text) {
    QStringList words;
    qsizetype index = 0;

    while (true) {
        while (index < text.size() && text[index].isSpace()) ++index;
        if (index == text.size()) break;
        if (text[index] != u'{') return std::nullopt;

        const qsizetype start = index + 1;
        int depth = 0;
        for (; index < text.size(); ++index) {
            if (text[index] == u'{') ++depth;
            if (text[index] == u'}' && --depth == 0) break;
        }
        if (index == text.size()) return std::nullopt;

        words.push_back(text.sliced(start, index - start).toString());
        ++index;
    }

    if (words.isEmpty()) return std::nullopt;
    return words;
}

eir::BitVector joinParts(std::span<const eir::BitVector> parts) {
    std::uint32_t total = 0;
    for (const eir::BitVector& part : parts) total += part.width();

    // Copied a word at a time: a group made by adding a subtree joins every signal in it, for
    // every value its trace prints, and a bit at a time was most of what a repaint of a large
    // design cost.
    eir::BitVector joined = eir::BitVector::filled(total, eir::Logic::Zero);
    std::uint32_t top = total;
    for (const eir::BitVector& part : parts) {
        top -= part.width();
        eir::bitops::assignPartAt(joined.mutableRef(), part.ref(), top);
    }
    return joined;
}

ValueReading joinReadings(std::span<const ValueReading> parts) {
    std::vector<eir::BitVector> values;
    values.reserve(parts.size());
    for (const ValueReading& part : parts) values.push_back(*part.value);
    ValueReading joined = accepted(joinParts(values));

    std::uint32_t top = joined.value->width();
    for (const ValueReading& part : parts) {
        top -= part.value->width();
        for (PartialDigit digit : part.partial) {
            digit.highBit += top;
            digit.lowBit += top;
            joined.partial.push_back(digit);
        }
    }
    return joined;
}

} // namespace enzan::gui
