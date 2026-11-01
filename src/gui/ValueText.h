// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/ArrayDimension.h"
#include "eir/BitVector.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace enzan::gui {

/// A digit the value column writes in capitals, `X` or `Z`: of the bits it covers, some are
/// unknown and not all, or, none of them unknown, some are high impedance and not all.
///
/// Such a digit is no one value of its bits but any of several, so a reading keeps it beside its
/// value rather than in it.
struct PartialDigit {
    std::uint32_t highBit = 0;
    std::uint32_t lowBit = 0;

    /// `Unknown` for an `X`, `HighZ` for a `Z`.
    eir::Logic kind = eir::Logic::Unknown;

    friend bool operator==(const PartialDigit&, const PartialDigit&) = default;
};

/// A value read from text a reader typed, or why none could be.
struct ValueReading {
    std::optional<eir::BitVector> value;

    /// The digits of `value` written `X` or `Z`. Their bits in `value` are x or z, as a lowercase
    /// digit would make them, and what they ask of a value is what each says rather than that.
    std::vector<PartialDigit> partial;

    /// Why there is no value, in a sentence for the reader. Empty when there is one.
    QString problem;
};

/// Reads `text` as a value of `width` bits, as the wave window's value column writes one in
/// `radix`: `ff` in hexadecimal, `1010` in binary, `200` in decimal, `AB` in ASCII.
///
/// The inverse of `eir::toString`, so that whatever a row shows can be typed back and means what
/// it showed. Bare digits are read as the literal `<width>'<radix><digits>` would be, so a
/// leading `x` covers every bit above it, as IEEE 1800 pads a literal; an ASCII row's text is its
/// characters, with the escapes the column writes, padded on the left with NUL bytes as a string
/// literal is.
///
/// **A literal means what it says in any radix**: `8'hff`, `'x` and `'1` are read as SystemVerilog
/// reads them, a fill literal filling the width and a narrower one extended with zeros, because a
/// literal is the one way of writing a value that states its own base. In an ASCII row a text
/// that is no literal is characters, so `it's` is four of them.
///
/// **A digit in capitals is what the column means by one**: `x` is a digit whose bits are all
/// unknown, and `X` one with some of them unknown and not all, so `1X` reads as a partial digit
/// (`ValueReading::partial`) rather than as `1x`. A digit of one bit is all or nothing, so there
/// the two are the same, and in a literal they are the same as SystemVerilog makes them.
///
/// **A value too wide for `width` is refused, never cut down**: a number cut to fit is another
/// number, and what was asked for would be silently replaced by something else.
[[nodiscard]] ValueReading readNumber(QStringView text, std::uint32_t width, eir::Radix radix);

/// Reads `text` as `renderArray` writes a value of `width` bits shaped by `dimensions`: the list
/// of its elements along the outermost, `{5}{a}`, each read by the dimensions below it, down to a
/// number. A number of the whole width is read too, as `readNumber` reads one.
[[nodiscard]] ValueReading readArray(QStringView text, std::uint32_t width,
                                     std::span<const eir::ArrayDimension> dimensions,
                                     eir::Radix radix);

/// The words a list is made of, `{a}{b{c}}` as `a` and `b{c}`, or nothing when `text` is no list:
/// braces that do not balance, or anything but space outside them.
[[nodiscard]] std::optional<QStringList> splitList(QStringView text);

/// `parts`, the first the most significant, as one value.
[[nodiscard]] eir::BitVector joinParts(std::span<const eir::BitVector> parts);

/// `parts`, the first the most significant, as one reading: their values joined, and their partial
/// digits moved to where their bits went. The first of them with no value is the reading, if one
/// has none.
[[nodiscard]] ValueReading joinReadings(std::span<const ValueReading> parts);

} // namespace enzan::gui
