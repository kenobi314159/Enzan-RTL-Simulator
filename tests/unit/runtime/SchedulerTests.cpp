// SPDX-License-Identifier: GPL-3.0-or-later
//
// The region iteration rules of DESIGN_GUIDE.md section 3.3 and the FIFO ordering of
// section 3.8 are what these tests pin. Both are semantics rather than implementation
// detail: a scheduler that runs the right processes in the wrong order produces
// plausible-looking but wrong results, which is the failure mode the project weighs
// above all others.

#include "runtime/Scheduler.h"

#include "eir/BitVector.h"

#include "support/Literals.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::runtime::NoTime;
using enzan::runtime::Region;
using enzan::runtime::Scheduler;
using enzan::test::literal;

/// Drains a region into a vector, so that ordering can be asserted directly.
std::vector<std::uint32_t> drain(Scheduler& scheduler, Region region) {
    std::vector<std::uint32_t> taken;
    while (!scheduler.empty(region)) taken.push_back(scheduler.take(region));
    return taken;
}

TEST(Scheduler, StartsEmpty) {
    const Scheduler scheduler;
    EXPECT_TRUE(scheduler.allRegionsEmpty());
    EXPECT_FALSE(scheduler.hasFutureEvents());
    EXPECT_FALSE(scheduler.hasNonBlockingUpdates());
    EXPECT_EQ(scheduler.nextEventTime(), NoTime);
    EXPECT_EQ(scheduler.deltaCount(), 0U);
}

TEST(Scheduler, RegionsAreStrictlyFifo) {
    // Execution order within a region is observable and must be reproducible across
    // runs (section 3.8), which rules out anything but insertion order.
    Scheduler scheduler;
    for (const std::uint32_t process : {7U, 3U, 11U, 3U})
        scheduler.schedule(Region::Active, process);

    EXPECT_EQ(drain(scheduler, Region::Active), std::vector<std::uint32_t>({7, 3, 11, 3}));
    EXPECT_TRUE(scheduler.empty(Region::Active));
}

TEST(Scheduler, RegionsAreIndependent) {
    Scheduler scheduler;
    scheduler.schedule(Region::Active, 1);
    scheduler.schedule(Region::Inactive, 2);
    scheduler.schedule(Region::Postponed, 3);

    EXPECT_FALSE(scheduler.allRegionsEmpty());
    EXPECT_EQ(drain(scheduler, Region::Active), std::vector<std::uint32_t>({1}));
    EXPECT_EQ(drain(scheduler, Region::Inactive), std::vector<std::uint32_t>({2}));
    EXPECT_EQ(drain(scheduler, Region::Postponed), std::vector<std::uint32_t>({3}));
    EXPECT_TRUE(scheduler.allRegionsEmpty());
}

TEST(Scheduler, PromotionPreservesOrderAndAppends) {
    // Promoting Inactive into Active is what makes `#0` run after everything already
    // Active rather than before it.
    Scheduler scheduler;
    scheduler.schedule(Region::Active, 1);
    scheduler.schedule(Region::Inactive, 2);
    scheduler.schedule(Region::Inactive, 3);

    scheduler.promote(Region::Inactive, Region::Active);

    EXPECT_TRUE(scheduler.empty(Region::Inactive));
    EXPECT_EQ(drain(scheduler, Region::Active), std::vector<std::uint32_t>({1, 2, 3}));
}

TEST(Scheduler, PromotingAnEmptyRegionIsHarmless) {
    Scheduler scheduler;
    scheduler.schedule(Region::Active, 1);
    scheduler.promote(Region::Inactive, Region::Active);

    EXPECT_EQ(drain(scheduler, Region::Active), std::vector<std::uint32_t>({1}));
}

TEST(Scheduler, QueueStorageIsReusedAcrossDrains) {
    // The queues are drained to empty many times per timestamp, so the head index must
    // return to the start rather than walking away from it over a long run.
    Scheduler scheduler;
    for (int round = 0; round < 100; ++round) {
        scheduler.schedule(Region::Active, static_cast<std::uint32_t>(round));
        EXPECT_EQ(scheduler.take(Region::Active), static_cast<std::uint32_t>(round));
        EXPECT_TRUE(scheduler.empty(Region::Active));
    }
}

// ---------------------------------------------------------------------------
// The time wheel
// ---------------------------------------------------------------------------

TEST(Scheduler, NextEventTimeIsTheEarliest) {
    Scheduler scheduler;
    scheduler.scheduleAt(100, 1);
    scheduler.scheduleAt(5, 2);
    scheduler.scheduleAt(50, 3);

    EXPECT_TRUE(scheduler.hasFutureEvents());
    EXPECT_EQ(scheduler.nextEventTime(), 5U);
}

TEST(Scheduler, ReleasingATimeMakesItsProcessesActiveInOrder) {
    Scheduler scheduler;
    scheduler.scheduleAt(10, 1);
    scheduler.scheduleAt(10, 2);
    scheduler.scheduleAt(20, 3);

    scheduler.releaseTime(10);

    EXPECT_EQ(drain(scheduler, Region::Active), std::vector<std::uint32_t>({1, 2}))
        << "insertion order within a timestamp must be preserved";
    EXPECT_EQ(scheduler.nextEventTime(), 20U);
}

TEST(Scheduler, ReleasingATimeWithNothingScheduledIsHarmless) {
    Scheduler scheduler;
    scheduler.scheduleAt(10, 1);
    scheduler.releaseTime(7);

    EXPECT_TRUE(scheduler.empty(Region::Active));
    EXPECT_EQ(scheduler.nextEventTime(), 10U);
}

TEST(Scheduler, DrainingTheWheelLeavesNoFutureEvents) {
    Scheduler scheduler;
    scheduler.scheduleAt(1, 1);
    scheduler.releaseTime(1);

    EXPECT_FALSE(scheduler.hasFutureEvents());
    EXPECT_EQ(scheduler.nextEventTime(), NoTime);
}

// ---------------------------------------------------------------------------
// Non-blocking updates
// ---------------------------------------------------------------------------

TEST(Scheduler, NonBlockingUpdatesKeepTheirValues) {
    Scheduler scheduler;
    const BitVector first = literal("8'hA5");
    const BitVector second = literal("4'b10xz");

    scheduler.appendNonBlocking(3, first.ref(), 0, false);
    scheduler.appendNonBlocking(4, second.ref(), 2, true);

    ASSERT_TRUE(scheduler.hasNonBlockingUpdates());
    const auto updates = scheduler.nonBlockingUpdates();
    ASSERT_EQ(updates.size(), 2U);

    EXPECT_EQ(updates[0].signalIndex, 3U);
    EXPECT_FALSE(updates[0].partial);
    EXPECT_EQ(
        enzan::eir::toString(scheduler.updateValue(updates[0]), enzan::eir::Radix::Hexadecimal),
        "a5");

    // Both planes survive the pool, so an update carrying X or Z is not flattened.
    EXPECT_EQ(updates[1].signalIndex, 4U);
    EXPECT_TRUE(updates[1].partial);
    EXPECT_EQ(updates[1].lowBit, 2U);
    EXPECT_EQ(enzan::eir::toString(scheduler.updateValue(updates[1]), enzan::eir::Radix::Binary),
              "10xz");
}

TEST(Scheduler, TwoPartialUpdatesToOneSignalAreBothKept) {
    // Collapsing them at schedule time would let the later one silently discard the
    // earlier one's bits, which is the bug this record shape exists to avoid.
    Scheduler scheduler;
    const BitVector low = literal("4'hF");
    const BitVector high = literal("4'h3");

    scheduler.appendNonBlocking(1, low.ref(), 0, true);
    scheduler.appendNonBlocking(1, high.ref(), 4, true);

    ASSERT_EQ(scheduler.nonBlockingUpdates().size(), 2U);
    EXPECT_EQ(scheduler.nonBlockingUpdates()[0].lowBit, 0U);
    EXPECT_EQ(scheduler.nonBlockingUpdates()[1].lowBit, 4U);
}

TEST(Scheduler, ClearingDiscardsEverythingAndCanBeReused) {
    Scheduler scheduler;
    const BitVector value = literal("8'h01");
    scheduler.appendNonBlocking(1, value.ref(), 0, false);
    scheduler.clearNonBlocking();

    EXPECT_FALSE(scheduler.hasNonBlockingUpdates());
    EXPECT_TRUE(scheduler.nonBlockingUpdates().empty());

    // The pool is reused, so an offset from a previous round must not be read back.
    const BitVector other = literal("8'hFF");
    scheduler.appendNonBlocking(2, other.ref(), 0, false);
    ASSERT_EQ(scheduler.nonBlockingUpdates().size(), 1U);
    EXPECT_EQ(enzan::eir::toString(scheduler.updateValue(scheduler.nonBlockingUpdates()[0]),
                                   enzan::eir::Radix::Hexadecimal),
              "ff");
}

TEST(Scheduler, WideUpdatesSurviveThePool) {
    Scheduler scheduler;
    const BitVector wide = literal("128'hDEAD_BEEF_0000_0000_1234_5678_9ABC_DEF0");
    const BitVector narrow = literal("8'h42");

    scheduler.appendNonBlocking(1, wide.ref(), 0, false);
    scheduler.appendNonBlocking(2, narrow.ref(), 0, false);

    const auto updates = scheduler.nonBlockingUpdates();
    EXPECT_EQ(
        enzan::eir::toString(scheduler.updateValue(updates[0]), enzan::eir::Radix::Hexadecimal),
        "deadbeef00000000123456789abcdef0");
    EXPECT_EQ(
        enzan::eir::toString(scheduler.updateValue(updates[1]), enzan::eir::Radix::Hexadecimal),
        "42");
}

TEST(Scheduler, TakingUpdatesLeavesThePendingListEmpty) {
    Scheduler scheduler;
    const BitVector value = literal("8'h5A");
    scheduler.appendNonBlocking(7, value.ref(), 0, false);

    const auto taken = scheduler.takeNonBlockingUpdates();
    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(taken[0].signalIndex, 7U);
    EXPECT_EQ(enzan::eir::toString(scheduler.takenValue(taken[0]), enzan::eir::Radix::Hexadecimal),
              "5a");
    EXPECT_FALSE(scheduler.hasNonBlockingUpdates());
}

TEST(Scheduler, AnUpdateAppendedWhileABatchIsAppliedLeavesTheBatchAlone) {
    // What a VPI callback does while the kernel applies the batch (DESIGN_GUIDE.md section
    // 9.4): the batch it is iterating must neither move nor change underneath it.
    Scheduler scheduler;
    const BitVector first = literal("8'h11");
    scheduler.appendNonBlocking(1, first.ref(), 0, false);

    const auto taken = scheduler.takeNonBlockingUpdates();
    const BitVector wide = literal("256'h1");
    for (std::uint32_t index = 0; index < 64; ++index)
        scheduler.appendNonBlocking(2, wide.ref(), 0, false);

    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(enzan::eir::toString(scheduler.takenValue(taken[0]), enzan::eir::Radix::Hexadecimal),
              "11");
    EXPECT_EQ(scheduler.nonBlockingUpdates().size(), 64U) << "the new ones wait for the next pass";
}

// ---------------------------------------------------------------------------
// Delta counting
// ---------------------------------------------------------------------------

TEST(Scheduler, DeltaCountingAndReset) {
    Scheduler scheduler;
    scheduler.countDelta();
    scheduler.countDelta();
    EXPECT_EQ(scheduler.deltaCount(), 2U);

    // The counter is per timestamp: a design that takes a few deltas at every one of a
    // million timestamps is perfectly healthy.
    scheduler.resetDeltaCount();
    EXPECT_EQ(scheduler.deltaCount(), 0U);
}

TEST(Scheduler, RegionNamesAreReadable) {
    EXPECT_EQ(enzan::runtime::toString(Region::Active), "Active");
    EXPECT_EQ(enzan::runtime::toString(Region::Nba), "NBA");
    EXPECT_EQ(enzan::runtime::toString(Region::ReNba), "Re-NBA");
    EXPECT_EQ(enzan::runtime::toString(Region::Postponed), "Postponed");
}

} // namespace
