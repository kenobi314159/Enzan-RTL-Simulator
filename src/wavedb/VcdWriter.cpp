// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/VcdWriter.h"

#include "common/Version.h"
#include "eir/BitVector.h"

#include <fstream>
#include <ostream>

namespace enzan::wavedb {

struct VcdWriter::Stream {
    std::ofstream file;
};

VcdWriter::VcdWriter(const runtime::Image& imageToRecord, const runtime::Arena& arena,
                     runtime::TimeScale timeScaleToUse)
    : WaveformWriter(imageToRecord, arena, timeScaleToUse), stream(std::make_unique<Stream>()) {}

VcdWriter::~VcdWriter() {
    // The base class cannot do this: by the time its destructor runs, the format hooks `close`
    // needs are already gone.
    close();
}

std::string VcdWriter::identifierFor(std::size_t index) {
    constexpr std::size_t Base = 94;
    constexpr char First = '!';

    std::string result;
    std::size_t remaining = index;
    do {
        result.push_back(static_cast<char>(First + (remaining % Base)));
        remaining /= Base;
    } while (remaining > 0);

    return result;
}

bool VcdWriter::openOutput(const std::filesystem::path& path, std::string& error) {
    stream->file.open(path, std::ios::out | std::ios::trunc);
    if (!stream->file.is_open()) {
        error = "could not be opened for writing";
        return false;
    }
    return true;
}

void VcdWriter::writeHeader() {
    identifiers.clear();
    identifiers.reserve(recordedSignals());
    for (std::size_t index = 0; index < recordedSignals(); ++index)
        identifiers.push_back(identifierFor(index));

    stream->file << "$version Enzan " << enzan::version() << " $end\n";
    stream->file << "$timescale 1" << timeScale().unitName() << " $end\n";
}

void VcdWriter::enterScope(std::string_view name) {
    stream->file << "$scope module " << name << " $end\n";
}

void VcdWriter::leaveScope() {
    stream->file << "$upscope $end\n";
}

void VcdWriter::declareSignal(std::uint32_t signalIndex, std::string_view leafName) {
    // `wire` for everything: GTKWave and Surfer do not distinguish, and the net/variable
    // distinction that does matter belongs to driver resolution (M5) rather than the waveform.
    //
    // Two names for one signal share an identifier, which VCD supports precisely for this.
    stream->file << "$var wire " << widthOf(signalIndex) << ' '
                 << identifiers[recordIndexOf(signalIndex)] << ' ' << leafName << " $end\n";
}

void VcdWriter::endDeclarations() {
    stream->file << "$enddefinitions $end\n";
}

void VcdWriter::beginInitialDump() {
    stream->file << "$dumpvars\n";
}

void VcdWriter::endInitialDump() {
    stream->file << "$end\n";
}

void VcdWriter::writeTimestamp(runtime::SimTime time) {
    stream->file << '#' << time << '\n';
}

void VcdWriter::writeChange(std::uint32_t signalIndex) {
    const eir::ConstBitRef value = valueOf(signalIndex);
    const std::string& identifier = identifiers[recordIndexOf(signalIndex)];

    if (value.width() == 1) {
        stream->file << eir::toChar(value.get(0)) << identifier << '\n';
        return;
    }

    // Vectors are emitted at full width. The format permits dropping leading zeroes, but a
    // constant width makes a diff of two VCD files line up, which is what section 14.3's
    // differential testing needs.
    stream->file << 'b' << eir::toString(value, eir::Radix::Binary) << ' ' << identifier << '\n';
}

void VcdWriter::closeOutput() {
    stream->file.flush();
    stream->file.close();
}

} // namespace enzan::wavedb
