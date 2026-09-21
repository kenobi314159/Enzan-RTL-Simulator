// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wavedb/WaveformWriter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace enzan::wavedb {

/// Writes value changes as a VCD stream.
///
/// VCD is the universal interchange format: human-readable, understood by every viewer, and
/// required both for interoperability and for the differential testing of DESIGN_GUIDE.md section
/// 14.3. Section 7.3 makes it available from M1 precisely because at M1 the external viewer *is*
/// the GUI.
///
/// Compared with FST it is large and slow to read, which is why section 7.3 lists both: VCD for
/// interchange and diffing, FST for anything big enough to care.
///
/// Everything that is not the format itself — selecting signals, walking the hierarchy, coalescing
/// changes within a time slot — lives in WaveformWriter.
class VcdWriter final : public WaveformWriter {
public:
    /// `image` and `arena` must outlive the writer.
    VcdWriter(const runtime::Image& image, const runtime::Arena& arena,
              runtime::TimeScale timeScale);

    ~VcdWriter() override;

    // The hooks keep the visibility they have in the base class. Narrowing an override to
    // private is legal but confusing: a reader of the base cannot then tell where it is
    // implemented, and clang-tidy rightly objects.
protected:
    bool openOutput(const std::filesystem::path& path, std::string& error) override;
    void writeHeader() override;
    void enterScope(std::string_view name) override;
    void leaveScope() override;
    void declareSignal(std::uint32_t signalIndex, std::string_view leafName) override;
    void endDeclarations() override;
    void beginInitialDump() override;
    void endInitialDump() override;
    void writeTimestamp(runtime::SimTime time) override;
    void writeChange(std::uint32_t signalIndex) override;
    void closeOutput() override;

private:
    /// Returns the short printable identifier for the recorded signal at `index`.
    ///
    /// VCD identifiers are printable ASCII from `!` to `~`, which gives 94 symbols. A base-94
    /// encoding keeps them one character for the first 94 signals and two for the next nine
    /// thousand, which is what makes a file for a large design mostly values rather than mostly
    /// identifiers.
    static std::string identifierFor(std::size_t index);

    /// Identifier per recorded signal, so that a collapsed port's two names share one.
    std::vector<std::string> identifiers;

    /// Held by pointer so that the stream type does not leak into every translation unit that
    /// records waveforms.
    struct Stream;
    std::unique_ptr<Stream> stream;
};

} // namespace enzan::wavedb
