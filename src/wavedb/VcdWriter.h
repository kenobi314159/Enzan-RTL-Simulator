// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::wavedb {

/// Writes value changes as a VCD stream.
///
/// VCD is the universal interchange format: human-readable, understood by every viewer,
/// and required both for interoperability and for the differential testing of
/// DESIGN_GUIDE.md section 14.3. Section 7.3 makes it available from M1 precisely because
/// at M1 the external viewer *is* the GUI.
///
/// This writer streams straight to a file rather than filling an in-memory database. The
/// database of section 7.1 — random access by time, a memory budget, disk spill — exists
/// to serve a GUI that redraws arbitrary zoom windows, and that arrives with M2. Writing
/// a database first and a file exporter second would mean carrying the memory cost of the
/// database in every batch run, which is the opposite of what a batch run wants.
///
/// **Values are read from the state arena at capture time** (section 7.2) rather than
/// copied when a change is reported. That is also what makes coalescing free: several
/// changes to one signal within a time slot collapse into the one value the slot settles
/// on, which is what every other simulator records and what keeps a diff of two VCD files
/// from drowning in delta-level noise.
class VcdWriter final : public runtime::ValueChangeListener {
public:
    /// `image` and `arena` must outlive the writer.
    VcdWriter(const runtime::Image& image, const runtime::Arena& arena,
              runtime::TimeScale timeScale);

    VcdWriter(const VcdWriter&) = delete;
    VcdWriter& operator=(const VcdWriter&) = delete;
    VcdWriter(VcdWriter&&) = delete;
    VcdWriter& operator=(VcdWriter&&) = delete;
    ~VcdWriter() override;

    /// Restricts recording to signals whose hierarchical path starts with `pathPrefix`.
    ///
    /// Selective capture is a requirement, not a convenience: on a 100k-LUT design,
    /// recording everything exhausts memory within seconds of simulated time (section
    /// 7.1). An empty prefix records the whole design, which is the right default only
    /// because the designs of M1 are small.
    ///
    /// Must be called before `open`.
    void setScope(std::string_view pathPrefix);

    /// Opens `path` and writes the header and the time-zero values.
    ///
    /// Call after the kernel's `initialize`, so that the initial values recorded are the
    /// ones the design starts from. Returns false having filled `error` on failure.
    bool open(const std::filesystem::path& path, std::string& error);

    /// Flushes the last time slot and closes the file. Called by the destructor.
    void close();

    [[nodiscard]] bool isOpen() const noexcept;

    /// Number of signals being recorded, for the transcript line that says so.
    [[nodiscard]] std::size_t recordedSignals() const noexcept { return recorded.size(); }

    void onValueChange(std::uint32_t signalIndex, eir::ConstBitRef value) override;

    void onTimeAdvance(runtime::SimTime time) override;

private:
    /// Emits `#time` and the value of every signal marked dirty, then clears the marks.
    void flushPendingTime();

    void writeValue(std::uint32_t signalIndex);

    void writeDeclarations();

    /// Assigns each recorded signal a short printable identifier, as VCD requires.
    static std::string identifierFor(std::size_t index);

    const runtime::Image* image;
    const runtime::Arena* arena;
    runtime::TimeScale timeScale;

    std::string scopePrefix;

    /// Signal index for each recorded signal, and the reverse mapping.
    std::vector<std::uint32_t> recorded;
    std::vector<std::string> identifiers;

    /// Recorded-signal index for each signal in the image, or `NotRecorded`.
    std::vector<std::uint32_t> recordIndex;

    /// Recorded signals whose value has changed in the pending time slot, and a flag per
    /// signal so that a signal changing many times in one slot is listed once.
    std::vector<std::uint32_t> dirty;
    std::vector<bool> isDirty;

    runtime::SimTime pendingTime = 0;
    bool headerWritten = false;

    /// Held by pointer so that the header can be `#include`-light and so that the stream
    /// type does not leak into every translation unit that records waveforms.
    struct Stream;
    std::unique_ptr<Stream> stream;

    static constexpr std::uint32_t NotRecorded = 0xFFFF'FFFFU;
};

} // namespace enzan::wavedb
