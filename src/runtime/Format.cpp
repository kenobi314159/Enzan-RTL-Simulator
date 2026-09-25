// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Format.h"

#include "eir/BitOps.h"

#include <cctype>
#include <cstddef>
#include <format>
#include <utility>

namespace enzan::runtime {

namespace {

/// Number of characters the widest value of `bitWidth` bits needs in `radix`.
std::size_t paddedWidth(std::uint32_t bitWidth, eir::Radix radix, bool isSigned) {
    switch (radix) {
    case eir::Radix::Binary: return bitWidth;
    case eir::Radix::Octal: return (bitWidth + 2) / 3;
    case eir::Radix::Hexadecimal: return (bitWidth + 3) / 4;
    case eir::Radix::Decimal: break;
    }

    // Decimal digits needed for 2^n - 1, computed as ceil(n * log10(2)) with integer
    // arithmetic: 643/2136 is log10(2) to more precision than 64 bits of width needs.
    const std::uint32_t magnitudeBits = isSigned && bitWidth > 0 ? bitWidth - 1 : bitWidth;
    const std::size_t digits = ((static_cast<std::size_t>(magnitudeBits) * 643) + 2135) / 2136;
    return (isSigned ? 1 : 0) + (digits == 0 ? 1 : digits);
}

/// Pads `text` on the left with spaces to `width`.
std::string padLeft(std::string text, std::size_t width) {
    if (text.size() >= width) return text;
    return std::string(width - text.size(), ' ') + text;
}

} // namespace

std::string toAsciiString(eir::ConstBitRef value) {
    std::string result;
    const std::uint32_t bytes = (value.width() + 7) / 8;
    result.reserve(bytes);

    // Most significant byte first, which is how a string literal is packed into a
    // vector.
    for (std::size_t index = bytes; index-- > 0;) {
        unsigned character = 0;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t position = (static_cast<std::uint32_t>(index) * 8) + bit;
            if (position < value.width() && value.get(position) == eir::Logic::One)
                character |= 1U << bit;
        }

        // Leading NULs come from zero-extending a literal into a wider vector; printing
        // them would put invisible control characters in the transcript.
        if (character == 0 && result.empty()) continue;
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string formatDefault(std::span<const FormatArgument> arguments) {
    std::string result;
    for (const FormatArgument& argument : arguments)
        result += eir::toString(argument.value, eir::Radix::Decimal, argument.isSigned);
    return result;
}

std::string formatSystemTask(std::string_view format, std::span<const FormatArgument> arguments,
                             const FormatContext& context) {
    std::string result;
    result.reserve(format.size() + 16);

    std::size_t nextArgument = 0;

    for (std::size_t index = 0; index < format.size(); ++index) {
        if (format[index] != '%') {
            result.push_back(format[index]);
            continue;
        }

        const std::size_t start = index;
        ++index;
        if (index >= format.size()) {
            // A trailing '%' is malformed; showing it beats swallowing it.
            result.push_back('%');
            break;
        }

        bool minimal = false;
        if (format[index] == '0') {
            minimal = true;
            ++index;
            if (index >= format.size()) {
                result.append(format.substr(start));
                break;
            }
        }

        const char conversion = format[index];

        if (conversion == '%') {
            result.push_back('%');
            continue;
        }

        if (conversion == 'm' || conversion == 'M') {
            result.append(context.scopeName);
            continue;
        }

        if (conversion == 't' || conversion == 'T') {
            // `%t` formats an argument, it does not read the clock: `$display("%t", $time)`
            // passes the time in. The current time is used only when the format string asks
            // for a time that was never supplied, which is a malformed call.
            if (nextArgument >= arguments.size()) {
                result.append(format.substr(start, index - start + 1));
                continue;
            }
            const FormatArgument& argument = arguments[nextArgument++];
            const SimTime value =
                argument.value.hasUnknown() ? context.time : argument.value.toUInt64();
            result += context.timeScale.format(value);
            continue;
        }

        eir::Radix radix = eir::Radix::Decimal;
        bool isString = false;
        switch (conversion) {
        case 'd':
        case 'D': radix = eir::Radix::Decimal; break;
        case 'h':
        case 'H':
        case 'x':
        case 'X': radix = eir::Radix::Hexadecimal; break;
        case 'b':
        case 'B': radix = eir::Radix::Binary; break;
        case 'o':
        case 'O': radix = eir::Radix::Octal; break;
        case 's':
        case 'S': isString = true; break;
        default:
            // An unrecognised conversion is copied through, so that a format string
            // Enzan does not understand is visible rather than silently dropped.
            result.append(format.substr(start, index - start + 1));
            continue;
        }

        if (nextArgument >= arguments.size()) {
            // Too few arguments is a mistake in the design; showing the unconsumed
            // conversion is more useful than aborting the run.
            result.append(format.substr(start, index - start + 1));
            continue;
        }

        const FormatArgument& argument = arguments[nextArgument++];

        if (isString) {
            result += toAsciiString(argument.value);
            continue;
        }

        std::string rendered = eir::toString(argument.value, radix, argument.isSigned);
        const bool uppercase = conversion == 'H' || conversion == 'X' || conversion == 'D'
                               || conversion == 'B' || conversion == 'O';
        if (uppercase) {
            for (char& character : rendered)
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }

        if (minimal) {
            result += rendered;
        } else {
            result += padLeft(std::move(rendered),
                              paddedWidth(argument.value.width(), radix, argument.isSigned));
        }
    }

    return result;
}

} // namespace enzan::runtime
