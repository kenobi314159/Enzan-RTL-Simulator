// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Scheduler.h"

#include "common/Assert.h"

#include <algorithm>
#include <utility>

namespace enzan::runtime {

std::string_view toString(Region region) noexcept {
    switch (region) {
    case Region::Active: return "Active";
    case Region::Inactive: return "Inactive";
    case Region::Nba: return "NBA";
    case Region::Observed: return "Observed";
    case Region::Reactive: return "Reactive";
    case Region::ReInactive: return "Re-Inactive";
    case Region::ReNba: return "Re-NBA";
    case Region::Postponed: return "Postponed";
    }
    return "Active";
}

void Scheduler::schedule(Region region, std::uint32_t processIndex) {
    queues[static_cast<std::size_t>(region)].push(processIndex);
}

bool Scheduler::empty(Region region) const noexcept {
    return queues[static_cast<std::size_t>(region)].empty();
}

std::uint32_t Scheduler::take(Region region) {
    Queue& queue = queues[static_cast<std::size_t>(region)];
    ENZAN_ASSERT(!queue.empty(), "take from an empty region");

    const std::uint32_t value = queue.pop();

    // Reclaim the storage as soon as the queue drains, so that the head index cannot
    // walk away from zero across a long run.
    if (queue.empty()) queue.clear();

    return value;
}

void Scheduler::putBack(Region region, std::uint32_t processIndex) {
    queues[static_cast<std::size_t>(region)].pushFront(processIndex);
}

void Scheduler::promote(Region from, Region to) {
    Queue& source = queues[static_cast<std::size_t>(from)];
    Queue& target = queues[static_cast<std::size_t>(to)];

    while (!source.empty()) target.push(source.pop());
    source.clear();
}

bool Scheduler::allRegionsEmpty() const noexcept {
    return std::ranges::all_of(queues, [](const Queue& queue) { return queue.empty(); });
}

void Scheduler::scheduleAt(SimTime time, std::uint32_t processIndex) {
    timeWheel[time].push_back(processIndex);
}

SimTime Scheduler::nextEventTime() const noexcept {
    return timeWheel.empty() ? NoTime : timeWheel.begin()->first;
}

void Scheduler::releaseTime(SimTime time) {
    const auto found = timeWheel.find(time);
    if (found == timeWheel.end()) return;

    for (const std::uint32_t processIndex : found->second) schedule(Region::Active, processIndex);

    timeWheel.erase(found);
}

void Scheduler::appendNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value,
                                  std::uint32_t lowBit, bool partial) {
    const auto offset = static_cast<std::uint32_t>(updatePool.size());
    updatePool.insert(updatePool.end(), value.avalWords().begin(), value.avalWords().end());
    updatePool.insert(updatePool.end(), value.bvalWords().begin(), value.bvalWords().end());

    pendingUpdates.push_back(NonBlockingUpdate{.signalIndex = signalIndex,
                                               .valueOffset = offset,
                                               .bitWidth = value.width(),
                                               .lowBit = lowBit,
                                               .partial = partial});
}

void Scheduler::clear() noexcept {
    for (Queue& queue : queues) queue.clear();
    timeWheel.clear();
    clearNonBlocking();
    deltas = 0;
}

void Scheduler::clearNonBlocking() {
    pendingUpdates.clear();
    updatePool.clear();
}

} // namespace enzan::runtime
