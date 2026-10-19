// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Time.h"

#include <format>

namespace enzan::runtime {

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
