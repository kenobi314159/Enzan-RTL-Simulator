// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/ChangePublisher.h"

#include <utility>

namespace enzan::ipc {

ChangePublisher::ChangePublisher(const runtime::Image& imageToWatch, const runtime::Arena& arena,
                                 runtime::TimeScale timeScaleToUse)
    : WaveformWriter(imageToWatch, arena, timeScaleToUse),
      subscribed(imageToWatch.signals().size(), false) {
    // A signal too wide for the database still streams once it is subscribed: the reader asked
    // for that one, and its trace from then on is all the window can show of it (section 7.1).
    setWidthLimit(0);
}

ChangePublisher::~ChangePublisher() {
    // Every concrete writer's destructor must close; the base class's cannot, because the
    // hooks it would need are gone by then.
    close();
}

void ChangePublisher::start() {
    std::string ignored;
    static_cast<void>(WaveformWriter::open(std::filesystem::path(), ignored));
}

void ChangePublisher::subscribe(std::span<const std::uint32_t> signals) {
    for (const std::uint32_t signalIndex : signals) {
        if (signalIndex >= subscribed.size() || subscribed[signalIndex]) continue;
        subscribed[signalIndex] = true;
        ++subscribedCount;
    }
}

void ChangePublisher::unsubscribe(std::span<const std::uint32_t> signals) {
    for (const std::uint32_t signalIndex : signals) {
        if (signalIndex >= subscribed.size() || !subscribed[signalIndex]) continue;
        subscribed[signalIndex] = false;
        --subscribedCount;
    }
}

std::vector<std::uint32_t> ChangePublisher::subscribedSignals() const {
    std::vector<std::uint32_t> signals;
    signals.reserve(subscribedCount);
    for (std::uint32_t signalIndex = 0; signalIndex < subscribed.size(); ++signalIndex) {
        if (subscribed[signalIndex]) signals.push_back(signalIndex);
    }
    return signals;
}

void ChangePublisher::clearSubscriptions() {
    subscribed.assign(subscribed.size(), false);
    subscribedCount = 0;
}

bool ChangePublisher::isSubscribed(std::uint32_t signalIndex) const {
    return signalIndex < subscribed.size() && subscribed[signalIndex];
}

eir::BitVector ChangePublisher::currentValue(std::uint32_t signalIndex) const {
    return eir::BitVector::from(valueOf(signalIndex));
}

std::vector<ValueChangesMessage> ChangePublisher::takeBatches() {
    // The slot being built is already complete by the time anyone asks. The base class writes
    // a timestamp and then that slot's changes in one go, inside the time advance that ended
    // it, so a caller between time slots never sees half of one.
    sealBatch();
    return std::exchange(ready, {});
}

void ChangePublisher::discard() {
    ready.clear();
    building.updates.clear();
}

void ChangePublisher::sealBatch() {
    if (building.updates.empty()) return;
    ready.push_back(std::exchange(building, {}));
}

bool ChangePublisher::openOutput(const std::filesystem::path& /*path*/, std::string& /*error*/) {
    return true;
}

void ChangePublisher::writeHeader() {}

void ChangePublisher::enterScope(std::string_view /*name*/) {}

void ChangePublisher::leaveScope() {}

void ChangePublisher::declareSignal(std::uint32_t /*signalIndex*/, std::string_view /*leafName*/) {}

void ChangePublisher::endDeclarations() {}

void ChangePublisher::writeTimestamp(runtime::SimTime time) {
    sealBatch();
    building.time = time;
}

void ChangePublisher::writeChange(std::uint32_t signalIndex) {
    if (!isSubscribed(signalIndex)) return;
    building.updates.push_back(
        ValueUpdate{.signalIndex = signalIndex, .value = currentValue(signalIndex)});
}

void ChangePublisher::closeOutput() {
    sealBatch();
}

} // namespace enzan::ipc
