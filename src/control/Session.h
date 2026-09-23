// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::codegen {
class CompiledDesign;
}

namespace enzan::eir {
class Design;
}

namespace enzan::elab {
class ElaboratedDesign;
}

namespace enzan::runtime {
class Image;
}

namespace enzan::wavedb {
class WaveformWriter;
}

namespace enzan::control {

/// Everything one `enzan` process holds between commands.
///
/// The session is the object the command vocabulary of section 8.1 acts on, and it is
/// deliberately not a Tcl thing: the GUI drives the same operations without an interpreter
/// anywhere in the picture, and VPI will too. A command's job is to parse its arguments and
/// call one of these methods (`src/control/README.md`).
///
/// The three phases of section 2.1 are three distinct states here, and the state machine is
/// what the commands enforce rather than discover:
///
/// - **analysed**: `vlog` has produced EIR. `vsim` needs this.
/// - **elaborated**: `vsim` has produced an instance tree, an arena and a kernel. Everything
///   that touches a signal needs this.
/// - **running**: between `run` calls. The kernel is stopped whenever a command sees it,
///   because commands are executed from the loop that drives the kernel and never from
///   inside it.
///
/// Every method reports through the diagnostic engine and returns a bool, rather than
/// throwing: an unanalysable design is an ordinary user error (section 13.2), and a script
/// that wants to carry on after one is entitled to.
class Session {
public:
    /// `sources` and `diagnostics` must outlive the session.
    Session(SourceManager& sourceManager, DiagnosticEngine& engine);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session();

    /// Where `$display`, `run`'s summary line and command output go. Null means standard
    /// output.
    void setTranscript(std::ostream* stream) noexcept;

    [[nodiscard]] std::ostream& transcriptStream() const;

    /// Sets the time unit delays are counted in. Takes effect at the next `vsim`.
    void setTimeScale(runtime::TimeScale scale) noexcept { scale_ = scale; }

    [[nodiscard]] runtime::TimeScale timeScale() const noexcept { return scale_; }

    /// Sets the delta cycles allowed at one timestamp. Takes effect at the next `vsim`.
    void setDeltaLimit(std::uint32_t limit) noexcept { deltaLimit = limit; }

    // -----------------------------------------------------------------------
    // Analysis — `vlog`
    // -----------------------------------------------------------------------

    /// What one `vlog` invocation adds.
    struct AnalysisRequest {
        std::vector<std::filesystem::path> files;
        std::vector<std::string> defines;
        std::vector<std::filesystem::path> includeDirectories;
    };

    /// Analyses `request` together with everything analysed before it.
    ///
    /// Together, rather than incrementally, because analysis currently hands every source to
    /// one slang compilation. A design that is really analysed once per unit and cached is
    /// section 4.4's library store; until that exists, re-analysing is the behaviour that is
    /// *correct*, and the only thing wrong with it is that it is slower than it will be.
    ///
    /// The files of every `vlog` so far are put in dependency order before analysis (section
    /// 8.3), so a script may name a package after the module that imports it.
    bool analyse(const AnalysisRequest& request);

    [[nodiscard]] bool hasDesign() const noexcept { return design != nullptr; }

    // -----------------------------------------------------------------------
    // Elaboration — `vsim`
    // -----------------------------------------------------------------------

    /// Elaborates `topModule`, compiles it and prepares a kernel at time zero.
    ///
    /// An empty `topModule` lets elaboration choose, which it does only when the design has
    /// exactly one candidate. Calling this again replaces the whole simulation, which is what
    /// `vsim` on a running design means everywhere else.
    bool elaborate(std::string_view topModule);

    [[nodiscard]] bool isElaborated() const noexcept { return kernel != nullptr; }

    // -----------------------------------------------------------------------
    // Running — `run`, `restart`
    // -----------------------------------------------------------------------

    /// Runs until `until`, or until the design stops when `until` is absent.
    ///
    /// `until` is an absolute time, not a duration: `run 100ns` is turned into an absolute
    /// limit by the command, because that is where the current time is known and where the
    /// unit conversion belongs.
    runtime::RunStatus run(std::optional<runtime::SimTime> until);

    /// Puts the design back to time zero, keeping the elaboration and the waveform files.
    bool restart();

    [[nodiscard]] runtime::SimTime time() const noexcept;

    /// The status of the last `run`.
    [[nodiscard]] runtime::RunStatus lastStatus() const noexcept { return status; }

    /// True once `$finish` or `$fatal` has ended the simulation.
    [[nodiscard]] bool isFinished() const noexcept;

    /// The status `$finish` or `$fatal` asked for.
    [[nodiscard]] int exitStatus() const noexcept;

    // -----------------------------------------------------------------------
    // Inspection — `examine`, `force`, `deposit`, `find`
    // -----------------------------------------------------------------------

    /// Returns the signal index for a hierarchical path, or `NoSignal`.
    [[nodiscard]] std::uint32_t findSignal(std::string_view path) const;

    static constexpr std::uint32_t NoSignal = 0xFFFF'FFFFU;

    /// Renders a signal's current value in `radix`.
    ///
    /// An enumerated signal whose value has a name is rendered by that name when `radix` is
    /// absent, matching what the wave window shows (section 10.3): a reader who asked what a
    /// state register holds wants `S_SEND`, and gets `3'h2` by asking for a radix.
    [[nodiscard]] std::string examine(std::uint32_t signalIndex,
                                      std::optional<eir::Radix> radix) const;

    /// Parses `literal` against a signal's width and writes it once.
    ///
    /// Returns false having filled `error` when the literal does not parse. The width is
    /// taken from the signal, so `deposit /tb/count 5` writes the right number of bits
    /// without the user counting them.
    bool deposit(std::uint32_t signalIndex, std::string_view literal, std::string& error);

    /// Parses `literal` and holds the signal at it until `release`.
    bool force(std::uint32_t signalIndex, std::string_view literal, std::string& error);

    /// Releases a forced signal, returning false when it was not forced.
    bool release(std::uint32_t signalIndex);

    /// Returns the paths of every signal matching a glob pattern, in declaration order.
    ///
    /// `*` matches any run of characters including `/`, and `?` matches one. Declaration
    /// order rather than sorted, because it is the order the design declares them in and
    /// section 3.8 requires an ordering that reproduces.
    [[nodiscard]] std::vector<std::string> findSignals(std::string_view pattern) const;

    /// Returns the paths of every instance matching a glob pattern, in elaboration order.
    [[nodiscard]] std::vector<std::string> findInstances(std::string_view pattern) const;

    // -----------------------------------------------------------------------
    // Waveforms — `log`, `vcd file`, `fst file`
    // -----------------------------------------------------------------------

    /// Restricts waveform recording to `path` and what is inside it. Empty records all.
    ///
    /// Applies to writers opened after it. One subtree rather than a set, which is what the
    /// writers support today (section 7.1); asking for a second one is refused rather than
    /// silently widened to the whole design.
    bool setWaveformScope(std::string_view path);

    [[nodiscard]] const std::string& waveformScope() const noexcept { return scope; }

    /// Opens a VCD file and records into it from now on. Returns false having reported.
    bool openVcd(const std::filesystem::path& path);

    /// Opens an FST file and records into it from now on. Returns false having reported.
    bool openFst(const std::filesystem::path& path);

    /// Finishes every open waveform file. Called by `quit` and by the destructor.
    void closeWaveforms();

    [[nodiscard]] std::size_t openWaveformFiles() const noexcept { return writers.size(); }

    // -----------------------------------------------------------------------
    // What the GUI and the tests reach for
    // -----------------------------------------------------------------------

    [[nodiscard]] runtime::Kernel* simulationKernel() const noexcept { return kernel.get(); }

    [[nodiscard]] const runtime::Image* designImage() const noexcept { return image.get(); }

private:
    /// Parses `literal` to the width of `signalIndex`, reporting through `error`.
    [[nodiscard]] std::optional<eir::BitVector> parseValue(std::uint32_t signalIndex,
                                                           std::string_view literal,
                                                           std::string& error) const;

    /// Opens `writer` on `path` and attaches it to the kernel.
    bool addWriter(std::unique_ptr<wavedb::WaveformWriter> writer,
                   const std::filesystem::path& path);

    SourceManager& sources;
    DiagnosticEngine& diagnostics;
    std::ostream* transcript = nullptr;

    /// Every source `vlog` has been given, in the order it was given them.
    std::vector<std::filesystem::path> files;
    std::vector<std::string> defines;
    std::vector<std::filesystem::path> includeDirectories;

    std::unique_ptr<eir::Design> design;
    std::unique_ptr<elab::ElaboratedDesign> elaborated;
    std::unique_ptr<codegen::CompiledDesign> compiled;
    std::unique_ptr<runtime::Image> image;
    std::unique_ptr<runtime::Kernel> kernel;

    /// Open waveform files. Owned here so that `quit` and the destructor both finish them.
    std::vector<std::unique_ptr<wavedb::WaveformWriter>> writers;

    std::string scope;
    runtime::TimeScale scale_;
    runtime::RunStatus status = runtime::RunStatus::Quiescent;
    std::uint32_t deltaLimit = 10'000;
};

} // namespace enzan::control
