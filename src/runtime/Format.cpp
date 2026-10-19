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
    case eir::Radix::Ascii: return (bitWidth + 7) / 8;
    case eir::Radix::Decimal: break;
    }

    // Decimal digits needed for 2^n - 1, computed as ceil(n * log10(2)) with integer
    // arithmetic: 643/2136 is log10(2) to more precision than 64 bits of width needs.
    const std::uint32_t magnitudeBits = isSigned && bitWidth > 0 ? bitWidth - 1 : bitWidth;
    const std::size_t digits = ((static_cast<std::size_t>(magnitudeBits) * 643) + 2135) / 2136;
    return (isSigned ? 1 : 0) + (digits == 0 ? 1 : digits);
}

/// Widens `text` to a field width: on the right when left-justified, otherwise on the left, with
/// zeros after any sign when `zeroPad` and with spaces when not. Text already at least as wide
/// is left alone, as a field width never truncates.
std::string widened(std::string text, std::optional<std::size_t> width, bool leftJustify,
                    bool zeroPad) {
    if (!width.has_value() || text.size() >= *width) return text;
    const std::size_t padding = *width - text.size();
    if (leftJustify) return text + std::string(padding, ' ');
    if (!zeroPad) return std::string(padding, ' ') + text;
    const std::size_t sign = !text.empty() && text.front() == '-' ? 1 : 0;
    return text.substr(0, sign) + std::string(padding, '0') + text.substr(sign);
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

        // An optional `-` to left-justify, then an optional field width (IEEE 1800 section
        // 21.2.1.3). A width of 0 asks for the minimal rendering; a width beginning with 0,
        // such as `%016x`, pads with zeros rather than spaces.
        bool leftJustify = false;
        if (format[index] == '-') {
            leftJustify = true;
            ++index;
        }
        std::optional<std::size_t> fieldWidth;
        bool zeroPad = false;
        if (index < format.size() && std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
            zeroPad = format[index] == '0';
            std::size_t width = 0;
            while (index < format.size()
                   && std::isdigit(static_cast<unsigned char>(format[index])) != 0) {
                width = (width * 10) + static_cast<std::size_t>(format[index] - '0');
                ++index;
            }
            fieldWidth = width;
        }
        if (index >= format.size()) {
            result.append(format.substr(start));
            break;
        }
        const bool minimal = fieldWidth.has_value() && *fieldWidth == 0;

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
            SimTime value = context.time;
            if (!argument.value.hasUnknown()) {
                value = argument.value.toUInt64();
                if (context.argumentUnit)
                    value = context.timeScale.ticksOf(value, *context.argumentUnit);
            }
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
            result += widened(toAsciiString(argument.value), fieldWidth, leftJustify, false);
            continue;
        }

        // The conversion's case changes nothing (IEEE 1800 section 21.2.1.2): `%H` is `%h`. The
        // case of a digit is what tells `x`, every bit unknown, from `X`, some of them.
        std::string rendered = eir::toString(argument.value, radix, argument.isSigned);

        // A decimal is rendered minimal and then widened; a radix whose digits each cover fixed
        // bits shows them all, and a field width only ever adds to that. Without a field width,
        // the value's own is the width, and `-` still justifies it to the left. The minimal
        // form of a radix drops its leading zero digits, keeping one; an unknown digit stays.
        if (minimal) {
            const std::size_t significant = rendered.find_first_not_of('0');
            rendered.erase(0, significant == std::string::npos ? rendered.size() - 1 : significant);
            result += rendered;
        } else if (fieldWidth.has_value()) {
            result += widened(std::move(rendered), fieldWidth, leftJustify, zeroPad);
        } else {
            result += widened(std::move(rendered),
                              paddedWidth(argument.value.width(), radix, argument.isSigned),
                              leftJustify, false);
        }
    }

    return result;
}

} // namespace enzan::runtime
