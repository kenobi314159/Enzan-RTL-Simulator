// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include "eir/BitVector.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using enzan::eir::BitVector;
using enzan::eir::Encoding;
using enzan::eir::Logic;
using enzan::eir::NetKind;
using enzan::eir::resolvesSingleDrivers;
using enzan::eir::undrivenValue;

TEST(NetKind, AnUndrivenBitIsZOrWhatTheNetIsPulledOrTiedTo) {
    for (const NetKind kind : {NetKind::Wire, NetKind::Tri, NetKind::WiredAnd, NetKind::WiredOr,
                               NetKind::TriAnd, NetKind::TriOr, NetKind::UWire}) {
        EXPECT_EQ(undrivenValue(kind), Logic::HighZ) << enzan::eir::toString(kind);
    }
    EXPECT_EQ(undrivenValue(NetKind::Tri0), Logic::Zero);
    EXPECT_EQ(undrivenValue(NetKind::Supply0), Logic::Zero);
    EXPECT_EQ(undrivenValue(NetKind::Tri1), Logic::One);
    EXPECT_EQ(undrivenValue(NetKind::Supply1), Logic::One);
}

TEST(NetKind, OnlyPulledAndSupplyNetsResolveASingleDriver) {
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Tri0));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Tri1));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Supply0));
    EXPECT_TRUE(resolvesSingleDrivers(NetKind::Supply1));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::Wire));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::WiredAnd));
    EXPECT_FALSE(resolvesSingleDrivers(NetKind::UWire));
}

TEST(NetKind, IsNamedByItsKeyword) {
    EXPECT_EQ(enzan::eir::toString(NetKind::WiredAnd), "wand");
    EXPECT_EQ(enzan::eir::toString(NetKind::TriOr), "trior");
    EXPECT_EQ(enzan::eir::toString(NetKind::UWire), "uwire");
}

TEST(Encoding, VhdlLogicShowsAsTheFourStateBitEachCodeStandsFor) {
    // U X 0 1 Z W L H -, a byte each, the left element highest (DESIGN_GUIDE.md section 3.6).
    BitVector codes = BitVector::filled(72, Logic::Zero);
    for (std::uint32_t position = 0; position < 9; ++position) {
        const std::uint32_t element = 8 - position;
        for (std::uint32_t bit = 0; bit < 8; ++bit) {
            if (((position >> bit) & 1U) != 0) codes.set((element * 8) + bit, Logic::One);
        }
    }
    ASSERT_EQ(enzan::eir::presentedWidth(Encoding::VhdlLogic, 72), 9U);
    BitVector shown(9);
    enzan::eir::present(Encoding::VhdlLogic, codes.ref(), shown.mutableRef());
    EXPECT_EQ(enzan::eir::toString(shown.ref(), enzan::eir::Radix::Binary), "xx01zx01x");
}

TEST(Encoding, VhdlBitShowsAsTwoStateBitsAndBitsAsThemselves) {
    const BitVector codes = BitVector::fromUInt64(0x0100, 16);
    BitVector shown(2);
    enzan::eir::present(Encoding::VhdlBit, codes.ref(), shown.mutableRef());
    EXPECT_EQ(enzan::eir::toString(shown.ref(), enzan::eir::Radix::Binary), "10");
    EXPECT_EQ(enzan::eir::presentedWidth(Encoding::Bits, 16), 16U);
}

} // namespace
