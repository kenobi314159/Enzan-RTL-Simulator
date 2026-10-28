// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Time.h"

#include <format>

namespace enzan::runtime {

std::optional<TimeExponent> parseTimeUnit(std::string_view name) noexcept {
    if (name == "s") return 0;
    if (name == "ms") return -3;
    if (name == "us") return -6;
    if (name == "ns") return -9;
    if (name == "ps") return -12;
    if (name == "fs") return -15;
    return std::nullopt;
}

std::optional<TimeLength> parseTimeLength(std::string_view text) noexcept {
    const auto isDigit = [](char character) { return character >= '0' && character <= '9'; };
    std::size_t digits = 0;
    while (digits < text.size() && isDigit(text[digits])) ++digits;
    std::size_t number = digits;
    if (number < text.size() && text[number] == '.') {
        ++number;
        while (number < text.size() && isDigit(text[number])) ++number;
    }
    // A fraction has digits on both sides of its point, and only a length with a unit has one: a
    // count of the run's unit is whole by what it is.
    const std::string_view unit = text.substr(number);
    const bool fraction = number != digits;
    if (digits == 0 || (fraction && number == digits + 1) || (fraction && unit.empty()))
        return std::nullopt;

    TimeLength length{.mantissa = 0, .exponent = 0, .inUnits = unit.empty()};
    for (const char character : text.substr(0, number)) {
        if (character == '.') continue;
        if (__builtin_mul_overflow(length.mantissa, 10U, &length.mantissa)
            || __builtin_add_overflow(length.mantissa, static_cast<unsigned>(character - '0'),
                                      &length.mantissa))
            return std::nullopt;
    }
    if (!length.inUnits) {
        const std::optional<TimeExponent> exponent = parseTimeUnit(unit);
        if (!exponent) return std::nullopt;
        const int decimals = fraction ? static_cast<int>(number - digits - 1) : 0;
        length.exponent = *exponent - decimals;
    }
    return length;
}

std::optional<RunLength> parseRunLength(std::string_view text) noexcept {
    if (text == "all") return RunLength{.length = TimeLength{}, .untilFinished = true};
    const std::optional<TimeLength> length = parseTimeLength(text);
    if (!length) return std::nullopt;
    return RunLength{.length = *length, .untilFinished = false};
}

LengthFit TimeScale::countOf(const TimeLength& length, SimTime& count) const noexcept {
    if (length.inUnits) {
        count = length.mantissa;
        return LengthFit::Exact;
    }
    // Ten of the run's unit for each power of ten the length's unit is coarser by, and a tenth for
    // each it is finer by, which has to leave nothing over.
    SimTime counted = length.mantissa;
    for (int shift = length.exponent - exponent; shift > 0; --shift) {
        if (__builtin_mul_overflow(counted, 10U, &counted)) return LengthFit::TooLong;
    }
    for (int shift = exponent - length.exponent; shift > 0; --shift) {
        if (counted % 10U != 0) return LengthFit::NotWhole;
        counted /= 10U;
    }
    count = counted;
    return LengthFit::Exact;
}

std::string TimeScale::unitName() const {
    switch (exponent) {
    case 0: return "s";
    case -3: return "ms";
    case -6: return "us";
    case -9: return "ns";
    case -12: return "ps";
    case -15: return "fs";
    default: break;
    }

    // A precision that is not a multiple of three is legal in the language but has no
    // conventional name, so it is shown as the exponent rather than rounded to
    // something that would misreport it.
    return std::format("1e{}s", exponent);
}

std::string TimeScale::format(SimTime time) const {
    return std::format("{}{}", time, unitName());
}

SimTime TimeScale::ticksOf(std::uint64_t count, TimeExponent unit) const noexcept {
    SimTime ticks = count;
    for (int shift = unit - exponent; shift > 0; --shift) {
        if (ticks > NoTime / 10) return NoTime;
        ticks *= 10;
    }

    // Twenty decimal digits are more than 64 bits hold, so a finer unit than that leaves none.
    const int finer = exponent - unit;
    if (finer <= 0) return ticks;
    if (finer >= 20) return 0;
    std::uint64_t divisor = 1;
    for (int shift = finer; shift > 0; --shift) divisor *= 10;
    const std::uint64_t remainder = ticks % divisor;
    return ticks / divisor + (remainder >= divisor - remainder ? 1 : 0);
}

} // namespace enzan::runtime
