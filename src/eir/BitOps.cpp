// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/BitOps.h"

#include "common/Assert.h"

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace enzan::eir::bitops {

namespace {

/// Asserts that every operand of a binary operation has the same width, and that
/// the destination does not overlap a source.
///
/// The overlap check is a debug-only pointer comparison. It exists because an
/// aliased destination silently corrupts carry-propagating operations, and the
/// resulting wrong value would surface many time steps later as a mysterious
/// mismatch.
void checkBinary([[maybe_unused]] BitRef dst, [[maybe_unused]] ConstBitRef a,
                 [[maybe_unused]] ConstBitRef b) noexcept {
    ENZAN_ASSERT(dst.width() == a.width() && dst.width() == b.width(),
                 "binary bit operations require equal widths");
    ENZAN_ASSERT(dst.aval() != a.aval() && dst.aval() != b.aval(),
                 "the destination of a bit operation must not alias a source");
}

void checkPredicate([[maybe_unused]] BitRef dst, [[maybe_unused]] ConstBitRef a,
                    [[maybe_unused]] ConstBitRef b) noexcept {
    ENZAN_ASSERT(dst.width() == 1, "a comparison produces a single bit");
    ENZAN_ASSERT(a.width() == b.width(), "comparison operands must have equal widths");
}

/// Writes a single Logic into a one-bit destination.
void setBit(BitRef dst, Logic value) noexcept {
    ENZAN_ASSERT(dst.width() == 1, "expected a single-bit destination");
    dst.set(0, value);
}

/// The three masks a four-state limb decomposes into.
struct LimbMasks {
    Word isZero;    ///< bits that are exactly 0
    Word isOne;     ///< bits that are exactly 1
    Word isUnknown; ///< bits that are X or Z
};

LimbMasks decompose(Word aval, Word bval) noexcept {
    return {.isZero = ~aval & ~bval, .isOne = aval & ~bval, .isUnknown = bval};
}

/// Writes the decomposed result of a bitwise operation into a limb pair.
void composeLimb(Word& aval, Word& bval, Word isOne, Word isUnknown) noexcept {
    // X is encoded (1,1) and 1 is (1,0), so the aval plane carries both.
    aval = isOne | isUnknown;
    bval = isUnknown;
}

/// True when all bits at or above `from` in `value` are zero. Used to decide
/// whether a shift amount or an index fits in 64 bits.
bool fitsInUInt64(ConstBitRef value) noexcept {
    if (value.width() <= BitsPerWord) return true;

    const std::span<const Word> words = value.avalWords();
    return std::all_of(words.begin() + 1, words.end(), [](Word word) { return word == 0; });
}

/// Extracts a shift amount or index, or reports that it cannot be used.
///
/// Returns false when the operand has any unknown bit, in which case the caller
/// makes the whole result X. A value too large to hold in 64 bits is clamped to
/// the maximum rather than rejected, because every use here compares it against a
/// width and a clamped maximum gives the same answer.
bool extractCount(ConstBitRef value, std::uint64_t& count) noexcept {
    if (value.hasUnknown()) return false;

    count = fitsInUInt64(value) ? value.aval()[0] : std::numeric_limits<std::uint64_t>::max();
    return true;
}

/// Two-state magnitude comparison of equal-width vectors.
/// Returns -1, 0 or 1 as `a` is less than, equal to, or greater than `b`.
int compareMagnitude(ConstBitRef a, ConstBitRef b) noexcept {
    for (std::size_t index = a.words(); index-- > 0;) {
        if (a.aval()[index] != b.aval()[index]) return a.aval()[index] < b.aval()[index] ? -1 : 1;
    }
    return 0;
}

/// Signed comparison, interpreting both operands as two's complement.
int compareSigned(ConstBitRef a, ConstBitRef b) noexcept {
    const bool aNegative = a.get(a.width() - 1) == Logic::One;
    const bool bNegative = b.get(b.width() - 1) == Logic::One;
    if (aNegative != bNegative) return aNegative ? -1 : 1;

    // Same sign: the magnitude comparison of the two's-complement patterns gives
    // the right answer, because adding the same bias to both preserves order.
    return compareMagnitude(a, b);
}

/// Adds `b` into `a`'s words in place, returning the carry out of the top limb.
Word addInto(std::span<Word> a, std::span<const Word> b) noexcept {
    Word carry = 0;
    for (std::size_t index = 0; index < a.size(); ++index) {
        const Word sum = a[index] + b[index];
        const Word carried = sum + carry;
        // Either addition can wrap, and both need detecting.
        const Word nextCarry =
            (sum < a[index] ? Word{1} : Word{0}) + (carried < sum ? Word{1} : Word{0});
        a[index] = carried;
        carry = nextCarry;
    }
    return carry;
}

/// Replaces `value` with its two's complement in place.
void negateInPlace(std::span<Word> value) noexcept {
    for (Word& word : value) word = ~word;

    Word carry = 1;
    for (Word& word : value) {
        const Word sum = word + carry;
        carry = sum < word ? 1 : 0;
        word = sum;
        if (carry == 0) break;
    }
}

/// Unsigned long division of `dividend` by `divisor`, both in `dst`'s width.
///
/// Restoring division, one bit per iteration. Deliberately simple: division is
/// rare in RTL, correctness at arbitrary width matters more than speed, and a
/// clever wide-division routine is a classic source of off-by-one defects.
/// `quotient` and `remainder` may be null when that result is not wanted.
void divideMagnitude(ConstBitRef dividend, ConstBitRef divisor, BitRef* quotient,
                     BitRef* remainder) {
    const std::uint32_t width = dividend.width();

    // The running remainder needs one extra bit of headroom for the shift-in.
    BitVector accumulator = BitVector::filled(width + 1, Logic::Zero);
    BitVector result = BitVector::filled(width, Logic::Zero);
    BitVector divisorWide = BitVector::filled(width + 1, Logic::Zero);
    for (std::uint32_t index = 0; index < width; ++index)
        divisorWide.set(index, divisor.get(index));

    for (std::size_t bit = width; bit-- > 0;) {
        // accumulator = (accumulator << 1) | dividend[bit]
        for (std::size_t index = width; index-- > 0;)
            accumulator.set(static_cast<std::uint32_t>(index + 1),
                            accumulator.get(static_cast<std::uint32_t>(index)));
        accumulator.set(0, dividend.get(static_cast<std::uint32_t>(bit)));

        if (compareMagnitude(accumulator.ref(), divisorWide.ref()) >= 0) {
            BitVector negated = BitVector::from(divisorWide.ref());
            negateInPlace(negated.mutableRef().avalWords());
            negated.mutableRef().canonicalize();
            addInto(accumulator.mutableRef().avalWords(), negated.ref().avalWords());
            accumulator.mutableRef().canonicalize();
            result.set(static_cast<std::uint32_t>(bit), Logic::One);
        }
    }

    if (quotient != nullptr) copy(*quotient, result.ref());
    if (remainder != nullptr) {
        BitVector truncated = BitVector::filled(width, Logic::Zero);
        for (std::uint32_t index = 0; index < width; ++index)
            truncated.set(index, accumulator.get(index));
        copy(*remainder, truncated.ref());
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

void copy(BitRef dst, ConstBitRef source) {
    ENZAN_ASSERT(dst.width() == source.width(), "copy requires equal widths");

    // std::ranges::copy handles the aliased case correctly for a forward copy of
    // identical ranges, which is the only aliasing this function permits.
    std::ranges::copy(source.avalWords(), dst.aval());
    std::ranges::copy(source.bvalWords(), dst.bval());
}

void extend(BitRef dst, ConstBitRef source, bool signExtend) {
    if (dst.width() == source.width()) {
        copy(dst, source);
        return;
    }

    // A word at a time rather than a bit at a time. This runs for every assignment whose two
    // sides differ in width, which in RTL is most of them (`count <= count + 1'b1` widens the
    // `1'b1`), and a bit at a time cost two calls per bit.
    const std::uint32_t sourceWords = source.words();
    const std::uint32_t dstWords = dst.words();
    const std::uint32_t shared = std::min(sourceWords, dstWords);
    std::copy_n(source.aval(), shared, dst.aval());
    std::copy_n(source.bval(), shared, dst.bval());

    if (dst.width() < source.width()) {
        // Truncation: the low words, less whatever of the last one lies above the width.
        dst.canonicalize();
        return;
    }

    // An unknown sign bit makes every extended bit unknown, which is the
    // pessimistic answer the standard requires.
    const auto fill =
        static_cast<std::uint8_t>(signExtend ? source.get(source.width() - 1) : Logic::Zero);
    const Word fillA = (fill & 1U) != 0 ? ~Word{0} : Word{0};
    const Word fillB = (fill & 2U) != 0 ? ~Word{0} : Word{0};

    // The source's last word is canonical, zero above its width, so the fill is ORed into it.
    // The mask is empty when the source ends exactly on a word.
    const Word above = ~topWordMask(source.width());
    dst.aval()[sourceWords - 1] |= fillA & above;
    dst.bval()[sourceWords - 1] |= fillB & above;
    std::fill(dst.aval() + sourceWords, dst.aval() + dstWords, fillA);
    std::fill(dst.bval() + sourceWords, dst.bval() + dstWords, fillB);
    dst.canonicalize();
}

void setUnknown(BitRef dst) {
    dst.fill(Logic::Unknown);
}

// ---------------------------------------------------------------------------
// Bitwise
// ---------------------------------------------------------------------------

void bitwiseAnd(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        const LimbMasks left = decompose(a.aval()[index], a.bval()[index]);
        const LimbMasks right = decompose(b.aval()[index], b.bval()[index]);

        // 0 dominates; 1 only when both are 1; everything else is unknown.
        const Word isZero = left.isZero | right.isZero;
        const Word isOne = left.isOne & right.isOne;
        composeLimb(dst.aval()[index], dst.bval()[index], isOne, ~isZero & ~isOne);
    }
    dst.canonicalize();
}

void bitwiseOr(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        const LimbMasks left = decompose(a.aval()[index], a.bval()[index]);
        const LimbMasks right = decompose(b.aval()[index], b.bval()[index]);

        const Word isOne = left.isOne | right.isOne;
        const Word isZero = left.isZero & right.isZero;
        composeLimb(dst.aval()[index], dst.bval()[index], isOne, ~isOne & ~isZero);
    }
    dst.canonicalize();
}

void bitwiseXor(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        // XOR has no dominating value, so any unknown input bit is an unknown
        // output bit.
        const Word isUnknown = a.bval()[index] | b.bval()[index];
        const Word isOne = (a.aval()[index] ^ b.aval()[index]) & ~isUnknown;
        composeLimb(dst.aval()[index], dst.bval()[index], isOne, isUnknown);
    }
    dst.canonicalize();
}

void bitwiseXnor(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        const Word isUnknown = a.bval()[index] | b.bval()[index];
        const Word isOne = ~(a.aval()[index] ^ b.aval()[index]) & ~isUnknown;
        composeLimb(dst.aval()[index], dst.bval()[index], isOne, isUnknown);
    }
    dst.canonicalize();
}

void bitwiseNot(BitRef dst, ConstBitRef a) {
    ENZAN_ASSERT(dst.width() == a.width(), "bitwiseNot requires equal widths");

    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        // Z carries no value to invert, so it becomes X like an X does.
        const Word isUnknown = a.bval()[index];
        const Word isOne = ~a.aval()[index] & ~isUnknown;
        composeLimb(dst.aval()[index], dst.bval()[index], isOne, isUnknown);
    }
    dst.canonicalize();
}

// ---------------------------------------------------------------------------
// Reduction and truth
// ---------------------------------------------------------------------------

Logic truthValue(ConstBitRef value) {
    // A set bit decides the condition whatever the unknown bits turn out to be,
    // so a vector with both a 1 and an X is true rather than unknown.
    bool anyOne = false;
    for (std::uint32_t index = 0; index < value.words(); ++index) {
        if ((value.aval()[index] & ~value.bval()[index]) != 0) {
            anyOne = true;
            break;
        }
    }
    if (anyOne) return Logic::One;

    return value.hasUnknown() ? Logic::Unknown : Logic::Zero;
}

void reduceAnd(BitRef dst, ConstBitRef a) {
    bool anyUnknown = false;
    for (std::uint32_t index = 0; index < a.width(); ++index) {
        const Logic bit = a.get(index);
        if (bit == Logic::Zero) {
            setBit(dst, Logic::Zero);
            return;
        }
        if (isUnknown(bit)) anyUnknown = true;
    }
    setBit(dst, anyUnknown ? Logic::Unknown : Logic::One);
}

void reduceOr(BitRef dst, ConstBitRef a) {
    bool anyUnknown = false;
    for (std::uint32_t index = 0; index < a.width(); ++index) {
        const Logic bit = a.get(index);
        if (bit == Logic::One) {
            setBit(dst, Logic::One);
            return;
        }
        if (isUnknown(bit)) anyUnknown = true;
    }
    setBit(dst, anyUnknown ? Logic::Unknown : Logic::Zero);
}

void reduceXor(BitRef dst, ConstBitRef a) {
    // One unknown bit makes the parity unknowable, so there is no early exit.
    if (a.hasUnknown()) {
        setBit(dst, Logic::Unknown);
        return;
    }

    Word parity = 0;
    for (std::uint32_t index = 0; index < a.words(); ++index) parity ^= a.aval()[index];

    // Fold the limb down to a single bit.
    parity ^= parity >> 32;
    parity ^= parity >> 16;
    parity ^= parity >> 8;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    setBit(dst, (parity & 1U) != 0 ? Logic::One : Logic::Zero);
}

void logicalAnd(BitRef dst, ConstBitRef a, ConstBitRef b) {
    const Logic left = truthValue(a);
    const Logic right = truthValue(b);

    if (left == Logic::Zero || right == Logic::Zero) {
        setBit(dst, Logic::Zero);
        return;
    }
    if (left == Logic::One && right == Logic::One) {
        setBit(dst, Logic::One);
        return;
    }
    setBit(dst, Logic::Unknown);
}

void logicalOr(BitRef dst, ConstBitRef a, ConstBitRef b) {
    const Logic left = truthValue(a);
    const Logic right = truthValue(b);

    if (left == Logic::One || right == Logic::One) {
        setBit(dst, Logic::One);
        return;
    }
    if (left == Logic::Zero && right == Logic::Zero) {
        setBit(dst, Logic::Zero);
        return;
    }
    setBit(dst, Logic::Unknown);
}

void logicalNot(BitRef dst, ConstBitRef a) {
    switch (truthValue(a)) {
    case Logic::Zero: setBit(dst, Logic::One); return;
    case Logic::One: setBit(dst, Logic::Zero); return;
    default: setBit(dst, Logic::Unknown); return;
    }
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

void add(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    // A carry out of an unknown bit can reach any higher bit, so no result bit is
    // trustworthy once either operand has one.
    if (a.hasUnknown() || b.hasUnknown()) {
        setUnknown(dst);
        return;
    }

    std::ranges::fill(dst.bvalWords(), 0);
    std::ranges::copy(a.avalWords(), dst.aval());
    addInto(dst.avalWords(), b.avalWords());
    dst.canonicalize();
}

void subtract(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown()) {
        setUnknown(dst);
        return;
    }

    std::ranges::fill(dst.bvalWords(), 0);
    std::ranges::copy(a.avalWords(), dst.aval());

    // a - b as a + (-b), with the negation done in the destination's width so that
    // the wrap-around matches.
    BitVector negated = BitVector::from(b);
    negateInPlace(negated.mutableRef().avalWords());
    negated.mutableRef().canonicalize();

    addInto(dst.avalWords(), negated.ref().avalWords());
    dst.canonicalize();
}

void multiply(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkBinary(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown()) {
        setUnknown(dst);
        return;
    }

    // Schoolbook multiplication on 32-bit halves, keeping only the low `width`
    // bits. Products are accumulated in 64-bit limbs, so each partial product must
    // fit: 32 x 32 does.
    const std::uint32_t halves = dst.words() * 2;
    std::vector<std::uint64_t> product(halves, 0);

    const auto halfOf = [](ConstBitRef value, std::uint32_t index) -> std::uint64_t {
        const Word limb = value.aval()[index / 2];
        return (index % 2 == 0) ? (limb & 0xFFFF'FFFFU) : (limb >> 32);
    };

    for (std::uint32_t left = 0; left < halves; ++left) {
        const std::uint64_t leftHalf = halfOf(a, left);
        if (leftHalf == 0) continue;

        std::uint64_t carry = 0;
        for (std::uint32_t right = 0; left + right < halves; ++right) {
            const std::uint64_t term =
                (leftHalf * halfOf(b, right)) + product[left + right] + carry;
            product[left + right] = term & 0xFFFF'FFFFU;
            carry = term >> 32;
        }
    }

    std::ranges::fill(dst.bvalWords(), 0);
    for (std::uint32_t index = 0; index < dst.words(); ++index) {
        const auto half = static_cast<std::size_t>(index) * 2;
        dst.aval()[index] = product[half] | (product[half + 1] << 32);
    }
    dst.canonicalize();
}

void divide(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned) {
    checkBinary(dst, a, b);

    // IEEE 1800 makes division by zero an X result rather than a trap: a design
    // that divides by zero for one cycle during reset is common, and aborting
    // would make the simulator unusable on real code.
    if (a.hasUnknown() || b.hasUnknown() || b.isZero()) {
        setUnknown(dst);
        return;
    }

    BitVector left = BitVector::from(a);
    BitVector right = BitVector::from(b);
    bool negateResult = false;

    if (isSigned) {
        if (a.get(a.width() - 1) == Logic::One) {
            negateInPlace(left.mutableRef().avalWords());
            left.mutableRef().canonicalize();
            negateResult = !negateResult;
        }
        if (b.get(b.width() - 1) == Logic::One) {
            negateInPlace(right.mutableRef().avalWords());
            right.mutableRef().canonicalize();
            negateResult = !negateResult;
        }
    }

    BitVector quotient = BitVector::filled(dst.width(), Logic::Zero);
    BitRef quotientRef = quotient.mutableRef();
    divideMagnitude(left.ref(), right.ref(), &quotientRef, nullptr);

    if (negateResult) {
        negateInPlace(quotient.mutableRef().avalWords());
        quotient.mutableRef().canonicalize();
    }
    copy(dst, quotient.ref());
}

void modulo(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned) {
    checkBinary(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown() || b.isZero()) {
        setUnknown(dst);
        return;
    }

    BitVector left = BitVector::from(a);
    BitVector right = BitVector::from(b);

    // The remainder takes the sign of the dividend, which is what IEEE 1800
    // specifies and what differs from a mathematical modulus.
    bool negateResult = false;
    if (isSigned) {
        if (a.get(a.width() - 1) == Logic::One) {
            negateInPlace(left.mutableRef().avalWords());
            left.mutableRef().canonicalize();
            negateResult = true;
        }
        if (b.get(b.width() - 1) == Logic::One) {
            negateInPlace(right.mutableRef().avalWords());
            right.mutableRef().canonicalize();
        }
    }

    BitVector remainder = BitVector::filled(dst.width(), Logic::Zero);
    BitRef remainderRef = remainder.mutableRef();
    divideMagnitude(left.ref(), right.ref(), nullptr, &remainderRef);

    if (negateResult) {
        negateInPlace(remainder.mutableRef().avalWords());
        remainder.mutableRef().canonicalize();
    }
    copy(dst, remainder.ref());
}

void negate(BitRef dst, ConstBitRef a) {
    ENZAN_ASSERT(dst.width() == a.width(), "negate requires equal widths");

    if (a.hasUnknown()) {
        setUnknown(dst);
        return;
    }

    std::ranges::fill(dst.bvalWords(), 0);
    std::ranges::copy(a.avalWords(), dst.aval());
    negateInPlace(dst.avalWords());
    dst.canonicalize();
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------

namespace {

/// Shared implementation of the three shifts.
///
/// `fill` is the value shifted in, and is computed by the caller so that an
/// arithmetic right shift can supply the sign bit — including an unknown one.
void shift(BitRef dst, ConstBitRef a, ConstBitRef amount, bool toLeft, Logic fill) {
    ENZAN_ASSERT(dst.width() == a.width(), "a shift does not change the width");

    std::uint64_t count = 0;
    if (!extractCount(amount, count)) {
        // The amount decides where every bit lands, so an unknown amount makes
        // every result bit unknown.
        setUnknown(dst);
        return;
    }

    const std::uint32_t width = dst.width();
    if (count >= width) {
        // Everything is shifted out. This is defined behaviour in the language and
        // matches hardware, unlike C's shift-by-width.
        dst.fill(fill);
        return;
    }

    const auto distance = static_cast<std::uint32_t>(count);

    // Written as an explicit per-bit loop rather than limb shifting because the two
    // planes must move together and clarity matters more here than the last factor
    // of two; the two-state fast path that will matter is in codegen, which emits a
    // native shift when it can prove the operand two-state.
    if (toLeft) {
        for (std::uint32_t index = width; index-- > 0;) {
            dst.set(index, index >= distance ? a.get(index - distance) : fill);
        }
    } else {
        for (std::uint32_t index = 0; index < width; ++index) {
            const std::uint32_t source = index + distance;
            dst.set(index, source < width ? a.get(source) : fill);
        }
    }
}

} // namespace

void shiftLeft(BitRef dst, ConstBitRef a, ConstBitRef amount) {
    shift(dst, a, amount, true, Logic::Zero);
}

void shiftRightLogical(BitRef dst, ConstBitRef a, ConstBitRef amount) {
    shift(dst, a, amount, false, Logic::Zero);
}

void shiftRightArithmetic(BitRef dst, ConstBitRef a, ConstBitRef amount) {
    shift(dst, a, amount, false, a.get(a.width() - 1));
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

void equal(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkPredicate(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown()) {
        setBit(dst, Logic::Unknown);
        return;
    }
    setBit(dst, std::ranges::equal(a.avalWords(), b.avalWords()) ? Logic::One : Logic::Zero);
}

void notEqual(BitRef dst, ConstBitRef a, ConstBitRef b) {
    equal(dst, a, b);
    if (dst.get(0) != Logic::Unknown)
        dst.set(0, dst.get(0) == Logic::One ? Logic::Zero : Logic::One);
}

void caseEqual(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkPredicate(dst, a, b);
    setBit(dst, a.identicalTo(b) ? Logic::One : Logic::Zero);
}

void caseNotEqual(BitRef dst, ConstBitRef a, ConstBitRef b) {
    checkPredicate(dst, a, b);
    setBit(dst, a.identicalTo(b) ? Logic::Zero : Logic::One);
}

void lessThan(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned) {
    checkPredicate(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown()) {
        setBit(dst, Logic::Unknown);
        return;
    }

    const int ordering = isSigned ? compareSigned(a, b) : compareMagnitude(a, b);
    setBit(dst, ordering < 0 ? Logic::One : Logic::Zero);
}

void lessEqual(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned) {
    checkPredicate(dst, a, b);

    if (a.hasUnknown() || b.hasUnknown()) {
        setBit(dst, Logic::Unknown);
        return;
    }

    const int ordering = isSigned ? compareSigned(a, b) : compareMagnitude(a, b);
    setBit(dst, ordering <= 0 ? Logic::One : Logic::Zero);
}

// ---------------------------------------------------------------------------
// Selection and splicing
// ---------------------------------------------------------------------------

void bitSelect(BitRef dst, ConstBitRef source, ConstBitRef index) {
    ENZAN_ASSERT(dst.width() == 1, "a bit select produces a single bit");

    std::uint64_t position = 0;
    if (!extractCount(index, position) || position >= source.width()) {
        // An out-of-range select is X by definition, not an error: designs index
        // out of range at run time often enough that aborting would be unusable.
        setBit(dst, Logic::Unknown);
        return;
    }
    setBit(dst, source.get(static_cast<std::uint32_t>(position)));
}

void partSelectAt(BitRef dst, ConstBitRef source, std::uint64_t lowIndex) {
    for (std::uint32_t index = 0; index < dst.width(); ++index) {
        const std::uint64_t position = lowIndex + index;
        dst.set(index, position < source.width() ? source.get(static_cast<std::uint32_t>(position))
                                                 : Logic::Unknown);
    }
}

void partSelect(BitRef dst, ConstBitRef source, ConstBitRef lowIndex) {
    std::uint64_t low = 0;
    if (!extractCount(lowIndex, low)) {
        setUnknown(dst);
        return;
    }
    partSelectAt(dst, source, low);
}

void assignPartAt(BitRef dst, ConstBitRef source, std::uint64_t lowIndex) {
    for (std::uint32_t index = 0; index < source.width(); ++index) {
        const std::uint64_t position = lowIndex + index;
        if (position < dst.width())
            dst.set(static_cast<std::uint32_t>(position), source.get(index));
    }
}

void assignPart(BitRef dst, ConstBitRef source, ConstBitRef lowIndex) {
    std::uint64_t low = 0;
    if (!extractCount(lowIndex, low)) {
        // The target of the write is unknown, so nothing may be written. Making
        // the whole vector X because one index bit was unknown would lose far more
        // information than dropping the assignment does.
        return;
    }
    assignPartAt(dst, source, low);
}

void conditional(BitRef dst, ConstBitRef condition, ConstBitRef whenTrue, ConstBitRef whenFalse) {
    ENZAN_ASSERT(dst.width() == whenTrue.width() && dst.width() == whenFalse.width(),
                 "the arms of a conditional must match the destination width");

    switch (truthValue(condition)) {
    case Logic::One: copy(dst, whenTrue); return;
    case Logic::Zero: copy(dst, whenFalse); return;
    default: break;
    }

    // Unknown selector: merge per bit. Where both arms agree the answer is known
    // whichever way the selector resolves, which is what stops an unknown reset
    // from turning a whole datapath X when both arms drive the same value.
    const std::uint32_t count = dst.words();
    for (std::uint32_t index = 0; index < count; ++index) {
        const Word sameA = ~(whenTrue.aval()[index] ^ whenFalse.aval()[index]);
        const Word sameB = ~(whenTrue.bval()[index] ^ whenFalse.bval()[index]);
        const Word agree = sameA & sameB;

        dst.aval()[index] = (whenTrue.aval()[index] & agree) | ~agree;
        dst.bval()[index] = (whenTrue.bval()[index] & agree) | ~agree;
    }
    dst.canonicalize();
}

} // namespace enzan::eir::bitops
