// SPDX-License-Identifier: GPL-3.0-or-later

#include "pli/VpiValue.h"

#include "eir/BitOps.h"

#include <cmath>
#include <format>
#include <string_view>

namespace enzan::pli {

namespace {

constexpr std::uint32_t BitsPerVecval = 32;

/// The low 64 bits of `bits`, with unknown bits read as 0 and the value sign-extended from its
/// width when `isSigned`.
std::uint64_t knownLowBits(eir::ConstBitRef bits, bool isSigned) {
    std::uint64_t raw = bits.aval()[0] & ~bits.bval()[0];
    const std::uint32_t width = bits.width();
    if (width >= 64) return raw;

    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    if (isSigned && (raw & sign) != 0) raw |= ~((sign << 1) - 1);
    return raw;
}

double toReal(eir::ConstBitRef bits, bool isSigned) {
    if (bits.width() <= 64) {
        const std::uint64_t raw = knownLowBits(bits, isSigned);
        return isSigned ? static_cast<double>(static_cast<std::int64_t>(raw))
                        : static_cast<double>(raw);
    }

    // Wider than a machine integer: accumulated a word at a time from the top, which loses
    // precision exactly where a double has to.
    double result = 0.0;
    for (std::uint32_t word = bits.words(); word-- > 0;) {
        result =
            std::ldexp(result, 64) + static_cast<double>(bits.aval()[word] & ~bits.bval()[word]);
    }
    return result;
}

/// Packs the vector eight bits to a character, most significant first, dropping the zero
/// bytes a value narrower than its declaration is padded with.
std::string toCharacters(eir::ConstBitRef bits) {
    const std::uint32_t bytes = (bits.width() + 7) / 8;
    std::string text;
    text.reserve(bytes);
    for (std::uint32_t byte = bytes; byte-- > 0;) {
        unsigned character = 0;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t index = (byte * 8) + bit;
            if (index < bits.width() && bits.get(index) == eir::Logic::One) character |= 1U << bit;
        }
        if (character != 0) text.push_back(static_cast<char>(character));
    }
    return text;
}

void toVecvals(eir::ConstBitRef bits, std::vector<s_vpi_vecval>& out) {
    const std::uint32_t count = (bits.width() + BitsPerVecval - 1) / BitsPerVecval;
    out.assign(count, s_vpi_vecval{.aval = 0, .bval = 0});
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t word = index / 2;
        const std::uint32_t shift = (index % 2) * BitsPerVecval;
        out[index].aval =
            static_cast<PLI_INT32>(static_cast<std::uint32_t>(bits.aval()[word] >> shift));
        out[index].bval =
            static_cast<PLI_INT32>(static_cast<std::uint32_t>(bits.bval()[word] >> shift));
    }
}

eir::Logic fromScalar(PLI_INT32 scalar, bool& ok) {
    ok = true;
    switch (scalar) {
    case vpi0:
    case vpiL: return eir::Logic::Zero;
    case vpi1:
    case vpiH: return eir::Logic::One;
    case vpiZ: return eir::Logic::HighZ;
    case vpiX:
    case vpiDontCare: return eir::Logic::Unknown;
    default: ok = false; return eir::Logic::Unknown;
    }
}

/// Parses digits written in `base` into `width` bits, by way of the literal syntax the rest of
/// Enzan already reads, so that there is one definition of what a digit string means.
std::optional<eir::BitVector> parseDigits(std::string_view digits, char base, std::uint32_t width,
                                          std::string& error) {
    std::optional<eir::BitVector> parsed =
        eir::BitVector::parse(std::format("{}'{}{}", width, base, digits));
    if (!parsed.has_value())
        error = std::format("'{}' is not a valid value in this format", digits);
    return parsed;
}

} // namespace

bool readValue(eir::ConstBitRef bits, bool isSigned, s_vpi_value& value, ValueStorage& storage,
               std::string& error) {
    const auto setString = [&](std::string text) {
        std::string& slot = storage.nextString();
        slot = std::move(text);
        value.value.str = slot.data();
        return true;
    };

    switch (value.format) {
    case vpiBinStrVal: return setString(eir::toString(bits, eir::Radix::Binary));
    case vpiOctStrVal: return setString(eir::toString(bits, eir::Radix::Octal));
    case vpiHexStrVal: return setString(eir::toString(bits, eir::Radix::Hexadecimal));
    case vpiDecStrVal: return setString(eir::toString(bits, eir::Radix::Decimal, isSigned));
    case vpiStringVal: return setString(toCharacters(bits));

    case vpiScalarVal:
        // Enzan's encoding of one bit is the standard's: 0, 1, Z and X are 0, 1, 2 and 3 in both.
        value.value.scalar = static_cast<PLI_INT32>(bits.get(0));
        return true;

    case vpiIntVal:
        value.value.integer =
            static_cast<PLI_INT32>(static_cast<std::uint32_t>(knownLowBits(bits, isSigned)));
        return true;

    case vpiRealVal: value.value.real = toReal(bits, isSigned); return true;

    case vpiVectorVal: {
        std::vector<s_vpi_vecval>& slot = storage.nextVector();
        toVecvals(bits, slot);
        value.value.vector = slot.data();
        return true;
    }

    case vpiObjTypeVal:
        value.format = bits.width() == 1 ? vpiScalarVal : vpiVectorVal;
        return readValue(bits, isSigned, value, storage, error);

    case vpiSuppressVal: return true;

    default:
        error = std::format("value format {} is not one Enzan can produce for a packed vector",
                            value.format);
        return false;
    }
}

std::optional<eir::BitVector> parseValue(const s_vpi_value& value, std::uint32_t width,
                                         std::string& error) {
    const auto text = [&]() -> std::optional<std::string_view> {
        if (value.value.str == nullptr) {
            error = "the value's string is a null pointer";
            return std::nullopt;
        }
        return std::string_view(value.value.str);
    };

    switch (value.format) {
    case vpiBinStrVal:
    case vpiOctStrVal:
    case vpiHexStrVal: {
        const std::optional<std::string_view> digits = text();
        if (!digits.has_value()) return std::nullopt;
        char base = 'h';
        if (value.format == vpiBinStrVal) base = 'b';
        if (value.format == vpiOctStrVal) base = 'o';
        return parseDigits(*digits, base, width, error);
    }

    case vpiDecStrVal: {
        std::optional<std::string_view> digits = text();
        if (!digits.has_value()) return std::nullopt;
        const bool negative = digits->starts_with('-');
        if (negative) digits->remove_prefix(1);

        std::optional<eir::BitVector> magnitude = parseDigits(*digits, 'd', width, error);
        if (!magnitude.has_value() || !negative) return magnitude;

        eir::BitVector negated = eir::BitVector::filled(width, eir::Logic::Zero);
        eir::bitops::negate(negated.mutableRef(), magnitude->ref());
        return negated;
    }

    case vpiScalarVal: {
        bool ok = false;
        const eir::Logic bit = fromScalar(value.value.scalar, ok);
        if (!ok) {
            error = std::format("{} is not a scalar value", value.value.scalar);
            return std::nullopt;
        }
        eir::BitVector result = eir::BitVector::filled(width, eir::Logic::Zero);
        result.set(0, bit);
        return result;
    }

    case vpiIntVal: return eir::BitVector::fromInt64(value.value.integer, width);

    case vpiRealVal: {
        if (!std::isfinite(value.value.real)) {
            error = "a real value that is not finite cannot be written to a vector";
            return std::nullopt;
        }
        return eir::BitVector::fromInt64(std::llround(value.value.real), width);
    }

    case vpiStringVal: {
        const std::optional<std::string_view> characters = text();
        if (!characters.has_value()) return std::nullopt;

        // Right-aligned, a character to eight bits, as a string literal is assigned.
        eir::BitVector result = eir::BitVector::filled(width, eir::Logic::Zero);
        std::uint32_t byte = 0;
        for (auto character = characters->rbegin(); character != characters->rend();
             ++character, ++byte) {
            const auto code = static_cast<unsigned char>(*character);
            for (std::uint32_t bit = 0; bit < 8; ++bit) {
                const std::uint32_t index = (byte * 8) + bit;
                if (index >= width) return result;
                result.set(index, ((code >> bit) & 1U) != 0 ? eir::Logic::One : eir::Logic::Zero);
            }
        }
        return result;
    }

    case vpiVectorVal: {
        if (value.value.vector == nullptr) {
            error = "the value's vector is a null pointer";
            return std::nullopt;
        }

        eir::BitVector result = eir::BitVector::filled(width, eir::Logic::Zero);
        const eir::BitRef target = result.mutableRef();
        const std::uint32_t count = (width + BitsPerVecval - 1) / BitsPerVecval;
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uint32_t word = index / 2;
            const std::uint32_t shift = (index % 2) * BitsPerVecval;
            target.aval()[word] |=
                static_cast<eir::Word>(static_cast<std::uint32_t>(value.value.vector[index].aval))
                << shift;
            target.bval()[word] |=
                static_cast<eir::Word>(static_cast<std::uint32_t>(value.value.vector[index].bval))
                << shift;
        }
        target.canonicalize();
        return result;
    }

    default:
        error = std::format("value format {} cannot be written to a packed vector", value.format);
        return std::nullopt;
    }
}

} // namespace enzan::pli
