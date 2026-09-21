// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/WaveformRecorder.h"

#include <string>

namespace enzan::wavedb {

WaveformRecorder::WaveformRecorder(const runtime::Image& imageToRecord, const runtime::Arena& arena,
                                   runtime::TimeScale timeScaleToUse,
                                   WaveformDatabase& databaseToFill)
    : WaveformWriter(imageToRecord, arena, timeScaleToUse), database(&databaseToFill) {}

WaveformRecorder::~WaveformRecorder() {
    // Every concrete writer's destructor must close, because the base class's cannot: the
    // format hooks it would need are already gone by the time it runs.
    close();
}

void WaveformRecorder::start() {
    std::string ignored;
    static_cast<void>(WaveformWriter::open(std::filesystem::path(), ignored));
}

bool WaveformRecorder::openOutput(const std::filesystem::path& /*path*/, std::string& /*error*/) {
    return true;
}

void WaveformRecorder::writeHeader() {}

void WaveformRecorder::enterScope(std::string_view /*name*/) {}

void WaveformRecorder::leaveScope() {}

void WaveformRecorder::declareSignal(std::uint32_t /*signalIndex*/, std::string_view /*leafName*/) {
    // A signal is declared once per hierarchical *name*, and a collapsed port has two. The
    // database keys on the signal rather than on the name, so the names are ignored here and
    // the signals are added in one pass below; the GUI resolves a path to a signal through the
    // elaborated hierarchy it is sent separately, which is where both names already live.
}

void WaveformRecorder::endDeclarations() {
    for (const std::uint32_t signalIndex : recordedIndices()) {
        database->addSignal(signalIndex, image().signals()[signalIndex].path, widthOf(signalIndex));
    }
}

void WaveformRecorder::writeTimestamp(runtime::SimTime time) {
    currentTime = time;
}

void WaveformRecorder::writeChange(std::uint32_t signalIndex) {
    const std::uint32_t entry = database->recordIndexOf(signalIndex);
    if (entry == WaveformDatabase::NotRecorded) return;

    database->append(entry, currentTime, valueOf(signalIndex));
}

void WaveformRecorder::closeOutput() {}

} // namespace enzan::wavedb
