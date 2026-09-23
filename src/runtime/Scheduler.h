// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Time.h"

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace enzan::runtime {

/// One region of the stratified event queue of IEEE 1800.
///
/// The order here is the order of DESIGN_GUIDE.md section 3.3:
///
/// ```
///   Preponed -> Active <-> Inactive <-> NBA -> Observed -> Reactive <-> Re-Inactive
///            <-> Re-NBA -> Postponed
/// ```
///
/// Preponed and the PLI-specific regions are not separate queues. Section 3.3 permits
/// representing them as callback hook points provided the callbacks fire at the right
/// point in the ordering, and nothing in the M1 subset samples in Preponed. The
/// enumerators for the Reactive family exist and iterate correctly even though only
/// `program` blocks and `$monitor` will put anything in them, because retrofitting a
/// region into a drained loop is how a scheduler ends up subtly wrong.
enum class Region : std::uint8_t {
    Active,
    Inactive,
    Nba,
    Observed,
    Reactive,
    ReInactive,
    ReNba,
    Postponed,
};

constexpr std::size_t RegionCount = 8;

std::string_view toString(Region region) noexcept;

/// A pending non-blocking assignment.
///
/// The value is held in a side pool rather than in the record, so that a wide vector
/// does not make every record wide and so that appending an update allocates nothing
/// once the pool has grown (DESIGN_GUIDE.md section 13.4).
struct NonBlockingUpdate {
    std::uint32_t signalIndex = 0;

    /// Offset of the value in the update pool.
    std::uint32_t valueOffset = 0;

    /// Width of the value being assigned, which is the signal's width for a whole
    /// assignment and narrower for a part assignment.
    std::uint32_t bitWidth = 0;

    /// Destination bit offset within the signal.
    std::uint32_t lowBit = 0;

    /// True when only `bitWidth` bits starting at `lowBit` are written.
    ///
    /// Partial updates are recorded rather than turned into a read-modify-write at
    /// schedule time, because two non-blocking assignments to different parts of one
    /// signal in the same time slot must both take effect; collapsing them would let the
    /// later one silently discard the earlier one's bits.
    bool partial = false;
};

/// The event queues of one simulation.
///
/// Holds no policy about what a process does — that is the kernel's job — only the
/// ordering. Keeping the two apart is what makes the region iteration rules of section
/// 3.3 testable on their own.
class Scheduler {
public:
    /// Adds `processIndex` to the end of `region`.
    ///
    /// Insertion is strictly FIFO. Execution order within a region is observable and
    /// must be reproducible across runs (section 3.8), so it must never depend on
    /// pointer values, allocation addresses or hash iteration order.
    void schedule(Region region, std::uint32_t processIndex);

    [[nodiscard]] bool empty(Region region) const noexcept;

    /// Removes and returns the first entry of `region`, which must not be empty.
    std::uint32_t take(Region region);

    /// Moves everything in `from` to the end of `to`, preserving order.
    void promote(Region from, Region to);

    /// True when no region holds anything.
    [[nodiscard]] bool allRegionsEmpty() const noexcept;

    /// Schedules `processIndex` to be made Active at `time`.
    void scheduleAt(SimTime time, std::uint32_t processIndex);

    /// The earliest time anything is scheduled for, or `NoTime`.
    [[nodiscard]] SimTime nextEventTime() const noexcept;

    /// Moves everything scheduled for `time` into the Active region.
    void releaseTime(SimTime time);

    [[nodiscard]] bool hasFutureEvents() const noexcept { return !timeWheel.empty(); }

    /// Appends a non-blocking update, copying `value` into the pool.
    void appendNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value, std::uint32_t lowBit,
                           bool partial);

    [[nodiscard]] std::span<const NonBlockingUpdate> nonBlockingUpdates() const noexcept {
        return pendingUpdates;
    }

    /// Returns a view of an update's value.
    [[nodiscard]] eir::ConstBitRef updateValue(const NonBlockingUpdate& update) const noexcept {
        return {updatePool.data() + update.valueOffset, update.bitWidth};
    }

    [[nodiscard]] bool hasNonBlockingUpdates() const noexcept { return !pendingUpdates.empty(); }

    /// Discards every pending update. Called once they have been applied.
    ///
    /// The pool's capacity is kept, so a steady-state simulation stops allocating for
    /// non-blocking assignments after the first few time slots.
    void clearNonBlocking();

    /// Number of delta cycles taken at the current timestamp.
    [[nodiscard]] std::uint32_t deltaCount() const noexcept { return deltas; }

    void countDelta() noexcept { ++deltas; }

    /// Resets the delta counter, which the kernel does whenever time advances.
    void resetDeltaCount() noexcept { deltas = 0; }

    /// Empties every region, the time wheel and the pending updates.
    ///
    /// What `restart` needs (DESIGN_GUIDE.md section 8.1): a design put back to time zero
    /// with a clock event still in the wheel from the previous run would resume at a
    /// timestamp the design has not reached, and a process still queued would run from a
    /// resume point belonging to a simulation that no longer exists.
    ///
    /// Capacity is kept, so a restarted simulation does not pay for its queues twice.
    void clear() noexcept;

private:
    /// A FIFO that reuses its storage.
    ///
    /// A vector with a head index rather than a deque: the queues are drained to empty
    /// many times per timestamp, so resetting an index is cheaper than releasing blocks,
    /// and the storage settles at the high-water mark instead of being reallocated.
    struct Queue {
        std::vector<std::uint32_t> entries;
        std::size_t head = 0;

        [[nodiscard]] bool empty() const noexcept { return head >= entries.size(); }

        void push(std::uint32_t value) { entries.push_back(value); }

        std::uint32_t pop() { return entries[head++]; }

        void clear() noexcept {
            entries.clear();
            head = 0;
        }
    };

    std::array<Queue, RegionCount> queues;

    /// Future events, ordered by time. An ordered map keeps both the time ordering and
    /// the insertion order within a timestamp, which together make the schedule
    /// reproducible. Section 6.2 calls for a hierarchical or calendar queue once there
    /// is a benchmark to justify it; this is the honest starting point.
    std::map<SimTime, std::vector<std::uint32_t>> timeWheel;

    std::vector<NonBlockingUpdate> pendingUpdates;
    std::vector<eir::Word> updatePool;

    std::uint32_t deltas = 0;
};

} // namespace enzan::runtime
