// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/FstWriter.h"

#include "common/Version.h"
#include "eir/BitVector.h"

#include "fstapi.h"

#include <string>

namespace enzan::wavedb {

FstWriter::FstWriter(const runtime::Image& imageToRecord, const runtime::Arena& arena,
                     runtime::TimeScale timeScaleToUse)
    : WaveformWriter(imageToRecord, arena, timeScaleToUse) {}

FstWriter::~FstWriter() {
    // The base class cannot do this: by the time its destructor runs, the format hooks `close`
    // needs are already gone.
    close();
}

bool FstWriter::openOutput(const std::filesystem::path& path, std::string& error) {
    // The second argument asks for a compressed hierarchy, which is most of why an FST of a large
    // design is small: the scope and variable names dominate a VCD header and compress well.
    context = fstWriterCreate(path.c_str(), 1);
    if (context == nullptr) {
        error = "could not be opened for writing";
        return false;
    }
    return true;
}

void FstWriter::writeHeader() {
    handles.assign(recordedSignals(), 0);

    // The library takes the precision as a power-of-ten exponent, which is exactly how Enzan
    // stores it (see runtime/Time.h), so there is no unit conversion to get wrong here.
    fstWriterSetTimescale(context, timeScale().precisionExponent());

    const std::string version = std::string("Enzan ") + std::string(enzan::version());
    fstWriterSetVersion(context, version.c_str());

    // The file type tells a viewer which language's conventions to display names under. VHDL and
    // mixed designs arrive at M6 and M7, and this becomes a property of the elaborated design
    // rather than a constant.
    fstWriterSetFileType(context, FST_FT_VERILOG);

    // LZ4 rather than zlib: it compresses a value-change block several times faster for a file a
    // few per cent larger, and a writer that slows the simulation down is a writer people turn
    // off. Section 7.1 wants recording to be affordable enough to leave on.
    fstWriterSetPackType(context, FST_WR_PT_LZ4);

    // Explicitly off. The parallel writer would compress blocks on another thread; the kernel is
    // single-threaded by design (section 6.4) and waveform output is byte-compared against a
    // reference, so a background thread would put determinism at risk for an unmeasured gain.
    fstWriterSetParallelMode(context, 0);
}

void FstWriter::enterScope(std::string_view name) {
    const std::string scopeName(name);
    fstWriterSetScope(context, FST_ST_VCD_MODULE, scopeName.c_str(), nullptr);
}

void FstWriter::leaveScope() {
    fstWriterSetUpscope(context);
}

void FstWriter::declareSignal(std::uint32_t signalIndex, std::string_view leafName) {
    const std::uint32_t record = recordIndexOf(signalIndex);
    const std::string name(leafName);

    // A non-zero alias handle tells the library this name is another view of a variable it already
    // knows, so the values are stored once. That is what a collapsed port needs: two real
    // hierarchical names for one piece of state.
    const fstHandle alias = handles[record];
    // A real is the format's own real, which holds the double rather than its bits.
    const fstHandle handle =
        fstWriterCreateVar(context, isReal(signalIndex) ? FST_VT_VCD_REAL : FST_VT_VCD_WIRE,
                           FST_VD_IMPLICIT, widthOf(signalIndex), name.c_str(), alias);

    if (alias == 0) handles[record] = handle;
}

void FstWriter::endDeclarations() {
    // Nothing to do: the library closes the hierarchy when the first value change arrives.
}

void FstWriter::writeTimestamp(runtime::SimTime time) {
    fstWriterEmitTimeChange(context, time);
}

void FstWriter::writeChange(std::uint32_t signalIndex) {
    if (isReal(signalIndex)) {
        const double value = realValueOf(signalIndex);
        fstWriterEmitValueChange(context, handles[recordIndexOf(signalIndex)], &value);
        return;
    }
    // The library takes one character per bit, most significant first — which is exactly what the
    // value engine's binary rendering produces, including `x` and `z`.
    valueBuffer = eir::toString(valueOf(signalIndex), eir::Radix::Binary);
    fstWriterEmitValueChange(context, handles[recordIndexOf(signalIndex)], valueBuffer.c_str());
}

void FstWriter::closeOutput() {
    if (context == nullptr) return;

    fstWriterClose(context);
    context = nullptr;
}

} // namespace enzan::wavedb
