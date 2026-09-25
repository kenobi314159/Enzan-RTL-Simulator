// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/SvIntConversion.h"

#include "slang/numeric/SVInt.h"

namespace enzan::frontend::sv {

eir::BitVector toBitVector(const slang::SVInt& value) {
    const std::uint32_t width = value.getBitWidth();
    eir::BitVector result = eir::BitVector::filled(width, eir::Logic::Zero);

    // Read one bit at a time through slang's own accessor rather than over its storage words.
    //
    // Reading the words directly is a little faster and considerably more fragile: it depends
    // on slang's internal layout, which is not a stability-guaranteed interface and is exactly
    // the kind of thing that changes under a rebase (DESIGN_GUIDE.md section 5.2 names that
    // risk for NVC, and it applies here too). It also made the static analyser — correctly —
    // unable to prove the indexing in bounds, because a narrow value is stored inline in a
    // single word.
    //
    // The cost is linear in the width, and this runs once per literal and initial value at
    // analysis time, never in the simulation loop.
    for (std::uint32_t index = 0; index < width; ++index) {
        const slang::logic_t bit = value[static_cast<std::int32_t>(index)];

        if (!bit.isUnknown()) {
            result.set(index, bit.value != 0 ? eir::Logic::One : eir::Logic::Zero);
            continue;
        }

        // slang distinguishes X from Z the same way Enzan does, but encodes the pair
        // differently in its value plane; asking it which one this is avoids having to know.
        result.set(index, bit == slang::logic_t::z ? eir::Logic::HighZ : eir::Logic::Unknown);
    }

    return result;
}

eir::BitVector toBitVector(std::string_view text) {
    const std::uint32_t width = text.empty() ? 8 : static_cast<std::uint32_t>(text.size()) * 8;
    eir::BitVector result = eir::BitVector::filled(width, eir::Logic::Zero);

    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[text.size() - 1 - index]);
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            if (((character >> bit) & 1U) != 0)
                result.set((static_cast<std::uint32_t>(index) * 8) + bit, eir::Logic::One);
        }
    }
    return result;
}

} // namespace enzan::frontend::sv
