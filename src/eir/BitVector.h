// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/Assert.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

/// The storage unit of a packed bit vector.
using Word = std::uint64_t;

constexpr std::uint32_t BitsPerWord = 64;

/// Returns the number of Words one bit plane of `bitWidth` bits occupies.
constexpr std::uint32_t wordsFor(std::uint32_t bitWidth) noexcept {
    return (bitWidth + BitsPerWord - 1) / BitsPerWord;
}

/// Returns the total number of Words a four-state vector of `bitWidth` bits
/// occupies, counting both planes.
constexpr std::uint32_t storageWordsFor(std::uint32_t bitWidth) noexcept {
    return 2 * wordsFor(bitWidth);
}

/// The widest literal `BitVector::parse` reads, which is the widest value slang
/// makes. A width past it is a typo rather than a value: made whole, it would
/// take gigabytes, and near 2^32 it overflows the count of Words it occupies.
constexpr std::uint32_t MaxLiteralWidth = (std::uint32_t{1} << 24) - 1;

/// Returns a mask of the bits used in the last Word of a plane of `bitWidth`
/// bits. A width that is a multiple of 64 uses the whole Word.
constexpr Word topWordMask(std::uint32_t bitWidth) noexcept {
    const std::uint32_t used = bitWidth % BitsPerWord;
    return used == 0 ? ~Word{0} : (Word{1} << used) - 1;
}

/// The four values a single bit of a packed vector can hold.
///
/// The numeric values are `aval | (bval << 1)`, which makes conversion between a
/// Logic and the two-plane encoding a shift rather than a table lookup, and which
/// matches VPI's `s_vpi_vecval` so that `vpi_get_value` and `vpi_put_value` stay
/// close to free (DESIGN_GUIDE.md section 3.6).
enum class Logic : std::uint8_t {
    Zero = 0,
    One = 1,
    HighZ = 2,
    Unknown = 3,
};

/// Returns the character IEEE 1800 uses for `value` in a binary rendering.
constexpr char toChar(Logic value) noexcept {
    switch (value) {
    case Logic::Zero: return '0';
    case Logic::One: return '1';
    case Logic::HighZ: return 'z';
    case Logic::Unknown: return 'x';
    }
    return 'x';
}

/// True when `value` is neither 0 nor 1.
constexpr bool isUnknown(Logic value) noexcept {
    return value == Logic::HighZ || value == Logic::Unknown;
}

// ---------------------------------------------------------------------------
// Views over packed storage
// ---------------------------------------------------------------------------
//
// Simulation values live in the state arena and are addressed by byte offset, so
// the operations below act on non-owning views rather than on objects that own
// memory. Nothing on the hot path allocates.
//
// A view's storage holds the `aval` plane first and the `bval` plane immediately
// after it, so a signal needs one arena offset rather than two.

/// Read-only view of a packed four-state bit vector.
///
/// **Canonical form.** Bits at or above `width()` MUST be zero in both planes.
/// Every operation here produces canonical output and relies on canonical input;
/// a stray high bit would make two equal values compare unequal and would leak
/// into a wider operation's result.
class ConstBitRef {
public:
    ConstBitRef() noexcept = default;

    /// `storage` must point at `storageWordsFor(bitWidth)` readable Words.
    ConstBitRef(const Word* storage, std::uint32_t bits) noexcept : store(storage), bitWidth(bits) {
        ENZAN_ASSERT(bits > 0, "a bit vector must have at least one bit");
        ENZAN_ASSERT(storage != nullptr, "a bit vector view needs storage");
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return bitWidth; }

    [[nodiscard]] std::uint32_t words() const noexcept { return wordsFor(bitWidth); }

    [[nodiscard]] const Word* aval() const noexcept { return store; }

    [[nodiscard]] const Word* bval() const noexcept { return store + words(); }

    [[nodiscard]] std::span<const Word> avalWords() const noexcept { return {aval(), words()}; }

    [[nodiscard]] std::span<const Word> bvalWords() const noexcept { return {bval(), words()}; }

    /// Returns the value of bit `index`, which must be less than `width()`.
    [[nodiscard]] Logic get(std::uint32_t index) const noexcept;

    /// True when no bit is X or Z, which is the case the two-state fast path
    /// exists for.
    [[nodiscard]] bool isTwoState() const noexcept;

    /// True when at least one bit is X or Z.
    [[nodiscard]] bool hasUnknown() const noexcept { return !isTwoState(); }

    /// True when every bit is exactly `value`.
    [[nodiscard]] bool isAll(Logic value) const noexcept;

    /// True when the vector is two-state and every bit is zero. This is the
    /// truth test a condition uses, negated.
    [[nodiscard]] bool isZero() const noexcept;

    /// Returns the low 64 bits of the `aval` plane, ignoring unknown bits.
    ///
    /// Intended for indices, shift amounts and delays, where the caller has
    /// already established that the value is two-state. Reading a vector with
    /// unknown bits through this silently treats X as the bit pattern it happens
    /// to carry, so callers MUST check `isTwoState()` first when the distinction
    /// matters.
    [[nodiscard]] std::uint64_t toUInt64() const noexcept;

    /// Returns the value sign-extended from `width()` to 64 bits.
    [[nodiscard]] std::int64_t toInt64() const noexcept;

    /// True when both planes are bit-identical to `other`, which must have the
    /// same width. This is SystemVerilog `===` over the whole vector.
    [[nodiscard]] bool identicalTo(ConstBitRef other) const noexcept;

private:
    const Word* store = nullptr;
    std::uint32_t bitWidth = 0;
};

/// Mutable view of a packed four-state bit vector. See ConstBitRef.
class BitRef {
public:
    BitRef() noexcept = default;

    BitRef(Word* storage, std::uint32_t bits) noexcept : store(storage), bitWidth(bits) {
        ENZAN_ASSERT(bits > 0, "a bit vector must have at least one bit");
        ENZAN_ASSERT(storage != nullptr, "a bit vector view needs storage");
    }

    /// Implicit, because every read-only operation accepts a ConstBitRef and
    /// requiring an explicit conversion at each call site would add noise without
    /// adding safety.
    // NOLINTNEXTLINE(google-explicit-constructor) — see above
    operator ConstBitRef() const noexcept { return {store, bitWidth}; }

    [[nodiscard]] std::uint32_t width() const noexcept { return bitWidth; }

    [[nodiscard]] std::uint32_t words() const noexcept { return wordsFor(bitWidth); }

    [[nodiscard]] Word* aval() const noexcept { return store; }

    [[nodiscard]] Word* bval() const noexcept { return store + words(); }

    [[nodiscard]] std::span<Word> avalWords() const noexcept { return {aval(), words()}; }

    [[nodiscard]] std::span<Word> bvalWords() const noexcept { return {bval(), words()}; }

    [[nodiscard]] Logic get(std::uint32_t index) const noexcept {
        return ConstBitRef(*this).get(index);
    }

    /// Sets bit `index`, which must be less than `width()`.
    void set(std::uint32_t index, Logic value) const noexcept;

    /// Sets every bit to `value`.
    void fill(Logic value) const noexcept;

    /// Zeroes the bits at and above `width()` in both planes, restoring canonical
    /// form. Operations that compute into the top Word call this; callers that
    /// write raw Words must call it themselves.
    void canonicalize() const noexcept;

private:
    Word* store = nullptr;
    std::uint32_t bitWidth = 0;
};

// ---------------------------------------------------------------------------
// Owning vectors
// ---------------------------------------------------------------------------

/// A packed four-state bit vector that owns its storage.
///
/// Used where a value is not in the state arena: literals from the frontend,
/// initial values computed during elaboration, arguments crossing the PLI
/// boundary, and test expectations. The simulation hot path uses BitRef into the
/// arena instead, because this type allocates.
class BitVector {
public:
    /// Constructs a vector of `bitWidth` bits, every bit X.
    ///
    /// X rather than 0 is the default because that is what an uninitialised
    /// four-state variable holds at time zero, and because a value that defaults
    /// to zero hides the bugs X exists to reveal.
    explicit BitVector(std::uint32_t bits);

    static BitVector filled(std::uint32_t bits, Logic value);

    /// Constructs from an unsigned value, truncating or zero-extending to
    /// `bitWidth`.
    static BitVector fromUInt64(std::uint64_t value, std::uint32_t bits);

    /// Constructs from a signed value, truncating or sign-extending to
    /// `bitWidth`.
    static BitVector fromInt64(std::int64_t value, std::uint32_t bits);

    /// Constructs from a copy of `source`.
    static BitVector from(ConstBitRef source);

    /// Parses a SystemVerilog-style literal such as `8'hff`, `4'b10x1`, `1'bz`,
    /// `16'd1234`, `'0`, `'1`, `'x`, `'z` or a plain decimal `42`.
    ///
    /// Returns nothing when the text is not a literal Enzan understands, rather
    /// than guessing: a misparsed literal is a wrong simulation result, and the
    /// caller has a source location to attach a diagnostic to.
    ///
    /// Digits short of the width are padded on the left with zeros, or with x or
    /// z when the leftmost digit is one, and digits beyond it are truncated, as
    /// IEEE 1800 section 5.7.1 specifies. A width past `MaxLiteralWidth` is
    /// refused.
    ///
    /// A plain decimal with no size is 32 bits wide and signed, as IEEE 1800
    /// specifies for an unsized decimal literal. An unsized `'0`/`'1`/`'x`/`'z`
    /// fill literal has no width of its own, so it is returned as one bit and the
    /// caller is expected to extend it in context.
    static std::optional<BitVector> parse(std::string_view text);

    [[nodiscard]] std::uint32_t width() const noexcept { return bitWidth; }

    [[nodiscard]] ConstBitRef ref() const noexcept { return {storage.data(), bitWidth}; }

    [[nodiscard]] BitRef mutableRef() noexcept { return {storage.data(), bitWidth}; }

    // NOLINTNEXTLINE(google-explicit-constructor) — the conversion is the point
    operator ConstBitRef() const noexcept { return ref(); }

    [[nodiscard]] Logic get(std::uint32_t index) const noexcept { return ref().get(index); }

    void set(std::uint32_t index, Logic value) noexcept { mutableRef().set(index, value); }

    /// Returns a copy resized to `newWidth`, zero-extending or sign-extending
    /// according to `signExtend`, or truncating.
    [[nodiscard]] BitVector resized(std::uint32_t newWidth, bool signExtend) const;

    friend bool operator==(const BitVector& left, const BitVector& right) noexcept;

private:
    std::uint32_t bitWidth = 0;
    std::vector<Word> storage;
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/// Number base used when rendering a vector, or `Ascii` for its bytes as characters.
enum class Radix : std::uint8_t { Binary, Octal, Decimal, Hexadecimal, Ascii };

/// Renders `value` in `radix`, following IEEE 1800's rules for unknown bits.
///
/// In binary, octal and hexadecimal each digit covers a fixed number of bits and
/// is rendered `x` or `z` when *all* of those bits are X or Z, and `X` or `Z` when
/// only some of them are. In decimal the whole value is `x` when every bit is X,
/// `z` when every bit is Z, `X` when some bits are X, and `Z` when some are Z and
/// none X, because a decimal rendering cannot localise the unknown to a digit.
///
/// Binary, octal and hexadecimal render every digit of the vector's width, which
/// is what a sized value displays as and what the waveform view needs. Decimal
/// renders the minimal number of digits, with a leading `-` when `isSigned` and
/// the value is negative. No width prefix is produced; the caller adds one where
/// it wants `8'h`.
///
/// `Ascii` renders a byte at a time, the most significant first, which is how a string
/// literal is packed into a vector. Leading NUL bytes are dropped, as `%s` drops them, since
/// they are what zero-extending a string into a wider vector adds. A printable character is
/// itself, and any other byte an escape: `\n`, `\t`, `\0`, `\\` or `\xNN`. Unlike `%s`, an
/// unknown byte is not read as zero: a byte with an X or Z bit is U+FFFD, the replacement
/// character, which no byte of a string can be mistaken for. The result is UTF-8.
std::string toString(ConstBitRef value, Radix radix, bool isSigned = false);

} // namespace enzan::eir
