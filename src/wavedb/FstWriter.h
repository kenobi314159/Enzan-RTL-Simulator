// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "wavedb/WaveformWriter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace enzan::wavedb {

/// Writes value changes as an FST file.
///
/// FST is "compressed, far more practical for large designs, readable by GTKWave and Surfer"
/// (DESIGN_GUIDE.md section 7.3), and it is what anyone recording a real design will actually use:
/// a VCD of a few million value changes is hundreds of megabytes where the FST is a few, and a
/// viewer can seek in it instead of parsing from the start.
///
/// The format is produced by GTKWave's own library, vendored in third_party/fst. Writing an FST
/// encoder from scratch would mean reimplementing a compressed block container with a hierarchy
/// blob and variable-length value encoding — a lot of work to arrive at a file a reader might
/// still reject. third_party/fst/README.md explains why this is the one case where vendoring is
/// unavoidable.
///
/// Everything that is not the format itself lives in WaveformWriter.
class FstWriter final : public WaveformWriter {
public:
    /// `image` and `arena` must outlive the writer.
    FstWriter(const runtime::Image& image, const runtime::Arena& arena,
              runtime::TimeScale timeScale);

    ~FstWriter() override;

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
    void writeTimestamp(runtime::SimTime time) override;
    void writeChange(std::uint32_t signalIndex) override;
    void closeOutput() override;

private:
    /// The library's opaque writer context. `void*` because that is the API's own type, and
    /// wrapping it in something stronger would mean a cast at every call.
    void* context = nullptr;

    /// The library's handle per recorded signal, or zero when not yet created.
    ///
    /// A collapsed port declares one signal under two names; the second declaration passes the
    /// first handle as an alias, so the file stores the values once. This vector is what remembers
    /// which case a declaration is.
    std::vector<std::uint32_t> handles;

    /// Reused buffer for the character-per-bit form the library takes, so that emitting a change
    /// allocates nothing (section 13.4).
    std::string valueBuffer;
};

} // namespace enzan::wavedb
