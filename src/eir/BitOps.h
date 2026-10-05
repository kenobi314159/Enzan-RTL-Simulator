// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"

#include <cstdint>

/// Four-state operations on packed bit vectors.
///
/// **This is the highest-risk code in the project.** Every one of these functions
/// implements a rule from IEEE 1800, and a subtly wrong X-propagation rule
/// produces plausible-looking but wrong simulation results — the failure mode
/// DESIGN_GUIDE.md section 1.2 ranks above all others, and the one users are least
/// able to detect. Changes here MUST come with tests that pin the behaviour.
///
/// **Calling conventions.**
///
/// - Binary operations require all three views to have the same width. The
///   frontend is responsible for inserting the extensions that SystemVerilog's
///   context-determined width rules call for, so that the runtime never has to
///   decide a width.
/// - `dst` MUST NOT overlap any source, except where a function's documentation
///   says otherwise. Codegen guarantees this by allocating distinct registers; the
///   alternative — copying into a temporary — would allocate on the hot path
///   (section 13.4).
/// - Every function leaves `dst` in canonical form: bits at or above its width are
///   zero in both planes.
/// - Nothing here allocates or throws.
///
/// **The two-state fast path.** Operations whose cost is dominated by
/// X-propagation bookkeeping check `isTwoState()` first and take a plain integer
/// path when they can. That is an optimisation of the four-state representation
/// and never a separate engine (section 3.6).
namespace enzan::eir::bitops {

// ---------------------------------------------------------------------------
// Movement
// ---------------------------------------------------------------------------

/// Copies `source` to `dst`, which must have the same width. `dst` may alias
/// `source`.
void copy(BitRef dst, ConstBitRef source);

/// Copies `source` into `dst` at any width, zero-extending, sign-extending or
/// truncating as required. `dst` may alias `source` only when the widths are
/// equal.
///
/// Sign-extending a vector whose top bit is X or Z fills the new bits with that
/// value, which is the pessimistic answer IEEE 1800 requires: a sign that is
/// unknown makes every extended bit unknown.
void extend(BitRef dst, ConstBitRef source, bool signExtend);

/// Sets every bit of `dst` to X. Used for the result of an operation whose
/// operands make it undefined, and for time-zero initialisation (section 3.9).
void setUnknown(BitRef dst);

// ---------------------------------------------------------------------------
// Bitwise, element by element
// ---------------------------------------------------------------------------

/// `dst = a & b`. Z is treated as X, per IEEE 1800: only `0 & anything` is 0 and
/// only `1 & 1` is 1.
void bitwiseAnd(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = a | b`.
void bitwiseOr(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = a ^ b`. Unknown in either operand makes the bit X, since XOR has no
/// dominating value.
void bitwiseXor(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = ~(a ^ b)`.
void bitwiseXnor(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = ~a`. Both X and Z invert to X, because Z carries no value to invert.
void bitwiseNot(BitRef dst, ConstBitRef a);

// ---------------------------------------------------------------------------
// Reduction and truth
// ---------------------------------------------------------------------------

/// Returns the single-bit truth value of `value`: One if any bit is 1, Zero if
/// every bit is 0, and Unknown otherwise.
///
/// This is what an `if`, a `while` and the `?:` selector test. Note that a vector
/// containing both a 1 and an X is *true*, not unknown: a set bit is enough to
/// decide the condition regardless of what the unknown bits turn out to be.
Logic truthValue(ConstBitRef value);

/// `dst`, one bit wide, receives the AND of every bit of `a`.
void reduceAnd(BitRef dst, ConstBitRef a);

/// `dst`, one bit wide, receives the OR of every bit of `a`.
void reduceOr(BitRef dst, ConstBitRef a);

/// `dst`, one bit wide, receives the XOR of every bit of `a`.
void reduceXor(BitRef dst, ConstBitRef a);

/// `dst`, one bit wide, receives `truthValue(a) && truthValue(b)`.
///
/// This is `&&`, not `&`: it is short-circuit in the language but not here,
/// because the bytecode compiler emits the branches that implement
/// short-circuiting and calls this only once both operands are available.
void logicalAnd(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `truthValue(a) || truthValue(b)`.
void logicalOr(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `!truthValue(a)`.
void logicalNot(BitRef dst, ConstBitRef a);

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------
//
// Arithmetic is all-or-nothing with respect to unknown bits: if any bit of either
// operand is X or Z, every bit of the result is X. IEEE 1800 requires this, and it
// is not merely pessimism — a carry can propagate from an unknown bit to any
// higher bit, so no result bit can be trusted.

/// `dst = a + b`, wrapping at the shared width.
void add(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = a - b`, wrapping at the shared width.
void subtract(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = a * b`, keeping the low `width` bits.
void multiply(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst = a / b`, truncating towards zero. Division by zero yields all X, which
/// is what IEEE 1800 specifies — not a trap and not zero.
void divide(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned);

/// `dst = a % b`, with the sign of `a` when signed. A zero divisor yields all X.
void modulo(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned);

/// `dst` receives how many bits of `a` are exactly 1, as a two-state number: what
/// `$countones` returns (IEEE 1800 section 20.9), with X and Z bits counted as not 1.
void countOnes(BitRef dst, ConstBitRef a);

/// `dst` receives the ceiling of the base-2 logarithm of `a`, read as unsigned, as a two-state
/// number: what `$clog2` returns (IEEE 1800 section 20.8.1), with 0 for an `a` of 0. An `a`
/// with an X or Z bit has no logarithm, and yields all X.
void ceilLog2(BitRef dst, ConstBitRef a);

/// `dst = base ** exponent`, which IEEE 1800 section 11.4.3 defines in full: X when either
/// operand has an unknown bit; for a negative exponent, which only a signed one can be, X for
/// a zero base, 1 for a base of 1, alternately -1 and 1 for a signed base of -1, and 0 for any
/// other; otherwise the power modulo 2 to the width. `dst` and `base` have one width, and the
/// exponent is self-determined.
void power(BitRef dst, ConstBitRef base, ConstBitRef exponent, bool baseSigned,
           bool exponentSigned);

/// `dst = -a`, two's complement, wrapping at the width.
void negate(BitRef dst, ConstBitRef a);

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------
//
// An unknown shift amount makes the whole result X: the amount decides where every
// bit lands, so none of them is known. A shift amount at or beyond the width
// shifts everything out rather than being undefined, which is what the language
// requires and what hardware does.

/// `dst = a << amount`, filling with zeroes.
void shiftLeft(BitRef dst, ConstBitRef a, ConstBitRef amount);

/// `dst = a >> amount`, filling with zeroes.
void shiftRightLogical(BitRef dst, ConstBitRef a, ConstBitRef amount);

/// `dst = a >>> amount`, filling with copies of the top bit of `a`.
void shiftRightArithmetic(BitRef dst, ConstBitRef a, ConstBitRef amount);

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

/// `dst`, one bit wide, receives `a == b`: 0 when a bit known on both sides differs,
/// and otherwise Unknown when either operand has an unknown bit, since that bit could
/// make the comparison go either way (IEEE 1800 section 11.4.5).
void equal(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a != b`.
void notEqual(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a === b`: an exact comparison of both planes
/// that distinguishes X from Z and never produces Unknown.
void caseEqual(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a !== b`.
void caseNotEqual(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a ==? b`: as `==`, except that an X or Z bit of `b` matches
/// anything (IEEE 1800 section 11.4.6). One of `a` is not a wildcard, and makes the result
/// Unknown where it is compared with a known bit of `b` that no known mismatch has settled.
void wildcardEqual(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a !=? b`.
void wildcardNotEqual(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives whether a `casez` item matches its selector: every bit that is
/// Z in neither is identical in both, X included (IEEE 1800 section 12.5.1). Never Unknown,
/// and symmetric, as the standard makes a Z in either side a don't-care.
void caseZMatch(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives whether a `casex` item matches its selector: every bit that is
/// X or Z in neither is equal in both. Never Unknown.
void caseXMatch(BitRef dst, ConstBitRef a, ConstBitRef b);

/// `dst`, one bit wide, receives `a < b`. Unknown when either operand has an
/// unknown bit.
void lessThan(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned);

/// `dst`, one bit wide, receives `a <= b`.
void lessEqual(BitRef dst, ConstBitRef a, ConstBitRef b, bool isSigned);

// ---------------------------------------------------------------------------
// Selection and splicing
// ---------------------------------------------------------------------------

/// `dst`, one bit wide, receives bit `index` of `source`, or X when `index` has
/// unknown bits or addresses past the end of `source`.
///
/// An out-of-range select is X rather than an error: IEEE 1800 defines it that
/// way, and a design that indexes out of range at run time is common enough that
/// aborting would make the simulator unusable.
void bitSelect(BitRef dst, ConstBitRef source, ConstBitRef index);

/// As `partSelect`, with the index as a plain integer.
///
/// The runtime often has the index in hand as a number rather than as a vector, and
/// wrapping it in a BitVector to pass it would allocate on the hot path (DESIGN_GUIDE.md
/// section 13.4).
void partSelectAt(BitRef dst, ConstBitRef source, std::uint64_t lowIndex);

/// As `assignPart`, with the index as a plain integer. See `partSelectAt`.
void assignPartAt(BitRef dst, ConstBitRef source, std::uint64_t lowIndex);

/// `dst` receives `dst.width()` bits of `source` starting at bit `lowIndex`.
///
/// Bits of the window that fall outside `source` read as X, and an unknown
/// `lowIndex` makes the whole result X.
void partSelect(BitRef dst, ConstBitRef source, ConstBitRef lowIndex);

/// Writes `source` into `dst` starting at bit `lowIndex`, leaving the rest of
/// `dst` unchanged.
///
/// Bits that would land outside `dst` are discarded. An unknown `lowIndex` leaves
/// `dst` completely unchanged, which is what IEEE 1800 requires for an assignment
/// through an unknown index: the target of the write is unknown, so nothing may be
/// written, and corrupting a whole vector because one index bit was X would be far
/// worse than dropping the assignment.
void assignPart(BitRef dst, ConstBitRef source, ConstBitRef lowIndex);

/// `dst = condition ? whenTrue : whenFalse`, with all three value widths equal.
///
/// When `condition` is unknown the result is computed per bit: a bit where
/// `whenTrue` and `whenFalse` agree takes that value, and a bit where they differ
/// becomes X. IEEE 1800 requires exactly this, and it matters in practice — it is
/// what stops an unknown reset from turning an entire datapath X when both arms
/// would have driven the same value.
void conditional(BitRef dst, ConstBitRef condition, ConstBitRef whenTrue, ConstBitRef whenFalse);

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------
//
// How a bit of a net with several drivers takes its value (DESIGN_GUIDE.md section 3.7). Each
// function folds one more driver into an accumulator that holds what the drivers so far
// resolved to, so a bit with `n` drivers costs `n - 1` calls. `acc` and `driver` must have the
// same width and must not overlap. Strength is not modelled: every driver is strong, which the
// frontend guarantees by refusing any other.
//
// `z` is the identity of all three, so an accumulator starts as the first driver's value.

/// `wire`, `tri` and `uwire`: a bit two drivers agree on keeps its value, `z` yields to the
/// other driver, and any other disagreement is X.
void resolveWire(BitRef acc, ConstBitRef driver);

/// `wand` and `triand`: 0 wins over everything but `z`, two 1s give 1, and X with anything
/// but 0 or `z` is X.
void resolveWiredAnd(BitRef acc, ConstBitRef driver);

/// `wor` and `trior`: the dual of `resolveWiredAnd`, with 1 winning.
void resolveWiredOr(BitRef acc, ConstBitRef driver);

/// Replaces each `z` bit of `value` with `pull`, which must be 0 or 1: what a `tri0` or `tri1`
/// does to a bit every driver leaves undriven.
void pullHighImpedance(BitRef value, Logic pull);

} // namespace enzan::eir::bitops
