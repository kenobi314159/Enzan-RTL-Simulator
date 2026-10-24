// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/UnitRequest.h"

#include <format>

namespace enzan::eir {

std::string realLiteral(double value) {
    std::string text = std::format("{:.17g}", value);
    if (text.find_first_of(".eEni") == std::string::npos) text += ".0";
    return text;
}

} // namespace enzan::eir
