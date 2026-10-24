// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "ipc/Protocol.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "runtime/Time.h"
#include "wavedb/WaveformWriter.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::ipc {

/// Turns a running kernel's value changes into messages for a client.
///
/// A third `WaveformWriter`, alongside the two exporters and the live database, whose "format"
/// is the socket. The reuse is the point: a slot's changes must collapse into the value it
/// settled on and must be attributed to the slot that ended, exactly as they do in a VCD, or
/// the GUI would draw transitions that no waveform file contains. Section 14.3 warns that two
/// correct simulators may differ at delta level; a GUI showing delta-level intermediate values
/// would have users reporting the display as a design bug.
///
/// Subscription is separate from the waveform database's logging scope. A signal the user drags
/// into the wave window streams live whether or not it is being recorded, and the client is
/// told which it is so that "nothing has happened yet" and "nothing is being kept" do not look
/// the same on screen.
class ChangePublisher final : public wavedb::WaveformWriter {
public:
    ChangePublisher(const runtime::Image& imageToWatch, const runtime::Arena& arena,
                    runtime::TimeScale timeScaleToUse);

    ~ChangePublisher() override;

    /// Begins watching. Call after the kernel's `initialize`, as for any other writer.
    void start();

    /// Adds signals to the streamed set. Unknown indices are ignored.
    void subscribe(std::span<const std::uint32_t> signals);

    void unsubscribe(std::span<const std::uint32_t> signals);

    [[nodiscard]] bool isSubscribed(std::uint32_t signalIndex) const;

    [[nodiscard]] std::size_t subscriptionCount() const noexcept { return subscribedCount; }

    /// Every subscribed signal, in index order. Ordered rather than hash-based because it
    /// survives a restart and decides the order the client is caught up in (section 3.8).
    [[nodiscard]] std::vector<std::uint32_t> subscribedSignals() const;

    /// Forgets every subscription, for a client that has just attached.
    void clearSubscriptions();

    /// The signal's value as it stands in the arena, for answering a fresh subscription.
    [[nodiscard]] eir::BitVector currentValue(std::uint32_t signalIndex) const;

    /// Removes and returns the batches accumulated since the last call, oldest first.
    [[nodiscard]] std::vector<ValueChangesMessage> takeBatches();

    /// Drops everything accumulated, for a restart.
    void discard();

private:
    // The database is the output, so the base class's file-shaped entry point is hidden.
    using wavedb::WaveformWriter::open;

    // Protected, matching the base class. Narrowing an override's visibility is legal and
    // confusing: a caller holding a `WaveformWriter&` can reach them either way.
protected:
    /// True: a signal the reader put in the window streams whatever it is, a memory too.
    [[nodiscard]] bool recordsArrays() const noexcept override { return true; }

    bool openOutput(const std::filesystem::path& path, std::string& error) override;
    void writeHeader() override;
    void enterScope(std::string_view name) override;
    void leaveScope() override;
    void declareSignal(std::uint32_t signalIndex, std::string_view leafName) override;
    void endDeclarations() override;
    void writeTimestamp(runtime::SimTime time) override;
    void writeChange(std::uint32_t signalIndex) override;
    void closeOutput() override;

private:
    /// Moves the batch being built into `ready`, if it holds anything.
    void sealBatch();

    /// One entry per signal in the image, rather than a set: this is tested once per recorded
    /// change in every settled slot, and a vector indexed by signal is the cheapest answer.
    std::vector<bool> subscribed;
    std::size_t subscribedCount = 0;

    std::vector<ValueChangesMessage> ready;
    ValueChangesMessage building;
};

} // namespace enzan::ipc
