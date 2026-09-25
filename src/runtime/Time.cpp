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

} // namespace enzan::runtime
