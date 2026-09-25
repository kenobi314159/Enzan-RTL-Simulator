// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include <compare>
#include <cstdint>
#include <string>

namespace enzan::eir {

/// The type of an EIR value.
///
/// A small value type rather than an interned pointer. Types in the M1 subset are
/// fully described by a kind, a width and two flags, so interning would add a
/// store, a lifetime and an indirection to save four bytes. When aggregate types
/// arrive (M5) this becomes a handle into a type store, and the accessors here are
/// the interface that change has to preserve.
class Type {
public:
    enum class Kind : std::uint8_t {
        /// No value. The type of a statement, and of a task call's result.
        Void,
        /// A packed vector of bits, possibly of width one.
        Bits,
        /// An IEEE 754 double. `real` and `shortreal` both map here; Enzan does not
        /// model the reduced precision of `shortreal`, and says so in the manual.
        Real,
        /// A character string. Held in the constant pool rather than the state
        /// arena in the M1 subset, which is enough for `$display` format strings.
        String,
    };

    constexpr Type() noexcept = default;

    static constexpr Type voidType() noexcept { return {}; }

    /// Constructs a packed vector type.
    ///
    /// `isFourState` records whether the *language* type can hold X and Z, which
    /// decides whether an assignment into it converts unknown bits to zero. It does
    /// not decide the storage layout: every Bits object carries both planes (see
    /// `storageWords`).
    static constexpr Type bits(std::uint32_t width, bool isSigned, bool isFourState) noexcept {
        Type result;
        result.typeKind = Kind::Bits;
        result.width = width;
        result.signedFlag = isSigned;
        result.fourStateFlag = isFourState;
        return result;
    }

    /// `logic` / `reg`: one bit, unsigned, four-state.
    static constexpr Type logic() noexcept { return bits(1, false, true); }

    /// `bit`: one bit, unsigned, two-state.
    static constexpr Type bit() noexcept { return bits(1, false, false); }

    /// `integer`: 32 bits, signed, four-state.
    static constexpr Type integer() noexcept { return bits(32, true, true); }

    /// `int`: 32 bits, signed, two-state.
    static constexpr Type intType() noexcept { return bits(32, true, false); }

    static constexpr Type real() noexcept {
        Type result;
        result.typeKind = Kind::Real;
        result.width = 64;
        result.signedFlag = true;
        return result;
    }

    static constexpr Type string() noexcept {
        Type result;
        result.typeKind = Kind::String;
        return result;
    }

    [[nodiscard]] constexpr Kind kind() const noexcept { return typeKind; }

    [[nodiscard]] constexpr bool isBits() const noexcept { return typeKind == Kind::Bits; }

    [[nodiscard]] constexpr bool isVoid() const noexcept { return typeKind == Kind::Void; }

    /// Width in bits. Meaningful for Bits and Real; zero for Void and String.
    [[nodiscard]] constexpr std::uint32_t bitWidth() const noexcept { return width; }

    /// True when arithmetic and comparison on this type treat the top bit as a
    /// sign. This is a property of the type, never of the stored bits.
    [[nodiscard]] constexpr bool isSigned() const noexcept { return signedFlag; }

    /// True when the language type can hold X and Z.
    ///
    /// Assigning an unknown value into a two-state object converts it to zero, as
    /// IEEE 1800 requires, so codegen has to know this at every assignment. It is
    /// also why the flag is part of the type rather than inferred from the bits: a
    /// two-state object whose bits happen to be known is still two-state.
    [[nodiscard]] constexpr bool isFourState() const noexcept { return fourStateFlag; }

    /// Number of Words this type occupies in the state arena.
    ///
    /// A Bits object always carries both bit planes, even when its language type is
    /// two-state. Uniform storage means one value path rather than two, which is the
    /// standing four-state-shape invariant (DESIGN_GUIDE.md section 17.4); the
    /// second plane of a two-state object stays zero, so the two-state fast path
    /// triggers on it for free. Packing two-state objects into one plane is a
    /// possible later optimisation and is not worth the second code path now.
    [[nodiscard]] constexpr std::uint32_t storageWords() const noexcept {
        switch (typeKind) {
        case Kind::Void:
        case Kind::String: return 0;
        case Kind::Bits: return storageWordsFor(width);
        case Kind::Real: return 1;
        }
        return 0;
    }

    /// Returns the type as it would be written in SystemVerilog, for diagnostics
    /// and for the GUI's object window.
    [[nodiscard]] std::string toString() const;

    friend constexpr bool operator==(Type, Type) noexcept = default;

private:
    Kind typeKind = Kind::Void;
    std::uint32_t width = 0;
    bool signedFlag = false;
    bool fourStateFlag = false;
};

} // namespace enzan::eir
