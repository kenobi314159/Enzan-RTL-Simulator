// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Module.h"

#include <gtest/gtest.h>

namespace {

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

} // namespace
