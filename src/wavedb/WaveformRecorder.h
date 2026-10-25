// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "runtime/Time.h"
#include "wavedb/WaveformDatabase.h"
#include "wavedb/WaveformWriter.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace enzan::wavedb {

/// Fills a WaveformDatabase from a running kernel.
///
/// This is `WaveformWriter` with the database standing in for a file format. That is not a
/// convenience: deciding which signals are in scope, walking the hierarchy to declare them,
/// collapsing the changes within a time slot into the value it settles on, and attributing
/// those changes to the slot that is *ending* are the same four problems for VCD, for FST and
/// for the live database, and they are where the subtle mistakes live. Section 7's database and
/// section 7.3's exporters must agree about every value at every timestamp, and the only way to
/// be sure of that is for them to share the code that decides it.
class WaveformRecorder final : public WaveformWriter {
public:
    /// `image`, `arena` and `database` must all outlive the recorder.
    WaveformRecorder(const runtime::Image& imageToRecord, const runtime::Arena& arena,
                     runtime::TimeScale timeScaleToUse, WaveformDatabase& databaseToFill);

    ~WaveformRecorder() override;

    /// Declares the in-scope signals to the database and records their time-zero values.
    ///
    /// Call after the kernel's `initialize` and after `setScope`, exactly as a file writer's
    /// `open` is called. Recording nothing until this runs is deliberate: a listener that
    /// started recording at construction would capture an arena that had not been initialised.
    void start();

    /// Records `signalIndex` from now on as well, beginning with the value it holds at `now`,
    /// whatever the scope and the width limit chose: what a reader puts in a wave window is
    /// recorded from the moment they do (DESIGN_GUIDE.md section 7.1). Nothing for a signal
    /// recorded already or one with no value, a VHDL signal of a null range.
    ///
    /// Between time slots only, as the kernel's listeners are called: `now` is the slot being
    /// recorded.
    void recordFromNow(std::uint32_t signalIndex, runtime::SimTime now);

private:
    // The database is the output, so there is no file to name and no failure to report. The
    // base class's file-shaped entry point is hidden rather than left callable, so that a
    // caller cannot ask a recorder to open a path and quietly get a recorder that ignored it.
    using WaveformWriter::open;

    // Protected, matching the base class. Narrowing an override's visibility is legal and
    // confusing: a caller holding a `WaveformWriter&` can reach them either way.
protected:
    /// True: the window shows an array as an array, and what a file leaves out it has to have.
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
    WaveformDatabase* database;

    /// The timestamp the changes now being written belong to.
    runtime::SimTime currentTime = 0;
};

} // namespace enzan::wavedb
