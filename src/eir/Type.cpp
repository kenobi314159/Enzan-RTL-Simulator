// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Type.h"

#include <format>

namespace enzan::eir {

std::string Type::toString() const {
    switch (typeKind) {
    case Kind::Void: return "void";
    case Kind::Real: return "real";
    case Kind::String: return "string";
    case Kind::Bits: break;
    }

    // Rendered as the user would have written it, so that a width mismatch
    // diagnostic quotes something recognisable rather than an internal encoding.
    const std::string_view base = fourStateFlag ? "logic" : "bit";
    const std::string_view sign = signedFlag ? " signed" : "";
    if (width == 1) return std::format("{}{}", base, sign);

    return std::format("{}{} [{}:0]", base, sign, width - 1);
}

} // namespace enzan::eir
