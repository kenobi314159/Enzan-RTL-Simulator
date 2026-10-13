// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::wavedb {

/// Everything a waveform writer does that has nothing to do with its file format.
///
/// Selecting which signals to record, walking the hierarchy to declare them, coalescing the
/// changes within a time slot, and attributing those changes to the right timestamp are the same
/// problems for VCD and for FST. They are also where the subtle mistakes live: the time a change
/// belongs to is the slot that is *ending* rather than the one beginning, and a scope filter that
/// compares paths with a plain prefix test puts `/tb/u_dut2` inside `/tb/u_dut`. Both of those were
/// real defects caught by tests, and having one copy of the answer is the point of this class.
///
/// A subclass supplies only the format: open a file, write a preamble, enter and leave a scope,
/// declare a signal, write a timestamp, write one value, close.
class WaveformWriter : public runtime::ValueChangeListener {
public:
    /// `image` and `arena` must outlive the writer.
    WaveformWriter(const runtime::Image& imageToRecord, const runtime::Arena& arena,
                   runtime::TimeScale timeScaleToUse);

    ~WaveformWriter() override;

    /// Restricts recording to signals whose hierarchical path is `pathPrefix` or lies inside it,
    /// replacing any selection already made.
    ///
    /// Selective capture is a requirement, not a convenience: on a 100k-LUT design, recording
    /// everything exhausts memory within seconds of simulated time (DESIGN_GUIDE.md section 7.1).
    /// An empty prefix records the whole design, which is the right default only because the
    /// designs of M1 are small.
    ///
    /// Must be called before `open`.
    void setScope(std::string_view pathPrefix);

    /// Adds `pathPrefix` to what is recorded, keeping what was already selected.
    ///
    /// A list rather than one prefix, because a selection is built out of subtrees: a request
    /// -r /tb/u_dut` names a subtree and `add wave /tb/clk` names one signal, and a reader
    /// builds up a selection out of both. A path that is a signal's own path matches that
    /// signal and nothing else, so the two cases need no distinction here.
    ///
    /// Adding an empty prefix records the whole design, which is what `log -r /*` means.
    ///
    /// Must be called before `open`.
    void addScope(std::string_view pathPrefix);

    /// Opens `path`, declares the recorded signals, and dumps their values at time zero.
    ///
    /// Call after the kernel's `initialize`, so that the initial values recorded are the ones the
    /// design starts from rather than an empty arena. Returns false having filled `error`.
    bool open(const std::filesystem::path& path, std::string& error);

    /// Writes out the time slot accumulated so far without closing.
    ///
    /// A slot's changes are normally written when the *next* time advance arrives, which is
    /// right for a file being streamed and wrong the moment a run pauses: the last thing the
    /// design did would be missing from the view until it was told to carry on. A caller that
    /// has stopped the kernel calls this so that what it shows is what has happened.
    ///
    /// Safe at any point between time slots, and a no-op when nothing is pending.
    void flush();

    /// Flushes the last time slot and closes the file.
    ///
    /// Safe to call more than once. Each concrete writer's destructor must call it: a base-class
    /// destructor cannot, because the format hooks it needs are already gone by then.
    void close();

    [[nodiscard]] bool isOpen() const noexcept { return opened; }

    /// Number of signals being recorded, for the transcript line that says so.
    [[nodiscard]] std::size_t recordedSignals() const noexcept { return recorded.size(); }

    void onValueChange(std::uint32_t signalIndex, eir::ConstBitRef value) final;

    void onTimeAdvance(runtime::SimTime time) final;

protected:
    [[nodiscard]] const runtime::Image& image() const noexcept { return *designImage; }

    [[nodiscard]] runtime::TimeScale timeScale() const noexcept { return scale; }

    /// The signal indices being recorded, in the order they were declared.
    [[nodiscard]] std::span<const std::uint32_t> recordedIndices() const noexcept {
        return recorded;
    }

    /// Position of `signalIndex` among the recorded signals, or `NotRecorded`.
    [[nodiscard]] std::uint32_t recordIndexOf(std::uint32_t signalIndex) const noexcept;

    /// Reads a signal's current value out of the state arena, as it is shown: a VHDL signal's
    /// codes converted to the bits they stand for (DESIGN_GUIDE.md section 3.6), and anything else
    /// as it is.
    ///
    /// Read at flush time rather than copied when the change was reported, which is what section
    /// 7.2 describes and what makes coalescing free. A converted value lives until the next call.
    [[nodiscard]] eir::ConstBitRef valueOf(std::uint32_t signalIndex) const;

    /// A signal's width as it is shown, which for VHDL's codes is one bit for each.
    [[nodiscard]] std::uint32_t widthOf(std::uint32_t signalIndex) const noexcept {
        const runtime::Signal& signal = designImage->signals()[signalIndex];
        return eir::presentedWidth(signal.encoding, signal.bitWidth);
    }

    static constexpr std::uint32_t NotRecorded = 0xFFFF'FFFFU;

    // --- format hooks ------------------------------------------------------

    /// Opens the output. Returns false having filled `error`.
    virtual bool openOutput(const std::filesystem::path& path, std::string& error) = 0;

    /// Writes whatever precedes the hierarchy: version, timescale, and so on.
    virtual void writeHeader() = 0;

    virtual void enterScope(std::string_view name) = 0;
    virtual void leaveScope() = 0;

    /// Declares one name for one signal.
    ///
    /// Called once per *name*, not once per signal: a collapsed port gives one slot two
    /// hierarchical names and both are real, so a format that can share storage between them
    /// should (see the VCD and FST writers, which both do).
    virtual void declareSignal(std::uint32_t signalIndex, std::string_view leafName) = 0;

    /// Called once the hierarchy is complete, before any value.
    virtual void endDeclarations() = 0;

    /// Brackets the full dump of every recorded signal at time zero. The default does nothing,
    /// for a format that needs no marker around it.
    virtual void beginInitialDump() {}

    virtual void endInitialDump() {}

    virtual void writeTimestamp(runtime::SimTime time) = 0;

    /// Writes the signal's current value, which `valueOf` supplies.
    virtual void writeChange(std::uint32_t signalIndex) = 0;

    virtual void closeOutput() = 0;

private:
    /// True when `path` lies in any selected scope, or when nothing has been selected.
    [[nodiscard]] bool isRecorded(std::string_view path) const;

    /// Chooses which signals to record, honouring the scope filter.
    void selectSignals();

    /// Walks the instance tree, calling the scope and declaration hooks.
    void declareHierarchy();

    /// Writes the pending timestamp and every signal marked dirty, then clears the marks.
    void flushPendingTime();

    const runtime::Image* designImage;
    const runtime::Arena* stateArena;

    /// Where `valueOf` converts a VHDL signal's codes.
    mutable eir::BitVector shown{1};
    runtime::TimeScale scale;

    /// What to record, or empty to record everything. Ordered, not a set: the declaration
    /// order of a waveform file is observable, and section 3.8 requires it to reproduce.
    std::vector<std::string> scopePrefixes;

    std::vector<std::uint32_t> recorded;

    /// Recorded position for each signal in the image, or `NotRecorded`.
    std::vector<std::uint32_t> recordIndex;

    /// Recorded signals changed in the pending slot, and a flag per recorded signal so that one
    /// changing many times within a slot is listed once.
    std::vector<std::uint32_t> dirty;
    std::vector<bool> isDirty;

    runtime::SimTime pendingTime = 0;
    bool opened = false;
};

} // namespace enzan::wavedb
