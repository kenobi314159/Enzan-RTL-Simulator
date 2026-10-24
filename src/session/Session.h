// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Library.h"
#include "elab/Elaborator.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
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

namespace enzan::frontend::vhdl {
struct DesignOptions;
}

namespace enzan::pli {
class PliContext;
}

namespace enzan::runtime {
class Image;
}

namespace enzan::wavedb {
class WaveformWriter;
}

namespace enzan::session {

/// The analysis, elaboration and simulation one `enzan` process holds.
///
/// One object for all three phases, because the batch driver and the served kernel must be the
/// same pipeline: two of them would mean two simulations, and a front end would then be showing
/// a different one from whatever else was driving it.
///
/// The three phases of section 2.1 are three distinct states here, and the state machine is
/// what the commands enforce rather than discover:
///
/// - **analysed**: `analyse` has produced EIR. Elaboration needs this.
/// - **elaborated**: `elaborate` has produced an instance tree, an arena and a kernel. Everything
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
    ///
    /// Takes effect immediately, including on a kernel that already exists: a served session
    /// sends its output to the attached client and the same session run from a script sends it
    /// to the terminal, and which of those is happening is not known when the kernel is built.
    void setTranscript(std::ostream* stream) noexcept;

    [[nodiscard]] std::ostream& transcriptStream() const;

    /// The engine every phase of this session reports through.
    ///
    /// Exposed so that a command which opens a file of its own — a file list, a do script —
    /// reports its failure exactly as an analysis failure is reported, through one engine
    /// (section 10.6) rather than as a bare string.
    [[nodiscard]] DiagnosticEngine& diagnosticEngine() const noexcept { return diagnostics; }

    /// Sets the time unit delays are counted in. Takes effect at the next elaboration.
    void setTimeScale(runtime::TimeScale unit) noexcept {
        scale = unit;
        defaultScale = unit;
    }

    /// Sets the time unit from the open libraries, or from `requested`, `--timescale`, when they
    /// hold no units; with neither, it is picoseconds.
    ///
    /// A library's delays are counts of the unit it was analysed in, so a design run from it,
    /// or analysed against it or into it, is in that unit and no other (DESIGN_GUIDE.md section
    /// 8.2). Returns false having reported when two libraries disagree, or `requested` names
    /// another unit: taking either would label every time in the run wrongly. Called once the
    /// libraries are open and before anything is analysed.
    [[nodiscard]] bool chooseTimeScale(std::optional<runtime::TimeScale> requested);

    [[nodiscard]] runtime::TimeScale timeScale() const noexcept { return scale; }

    /// Sets where VHDL's standard libraries are, for a program that does not keep them beside
    /// itself; by default they are where `frontend::vhdl::standardLibraryDirectory` says.
    void setVhdlLibraryDirectory(std::filesystem::path directory) {
        vhdlLibraries = std::move(directory);
    }

    /// Sets the delta cycles allowed at one timestamp. Takes effect at the next elaboration.
    void setDeltaLimit(std::uint32_t limit) noexcept { deltaLimit = limit; }

    /// Sets the severity, as a position of VHDL's `severity_level`, at which a report ends the
    /// run; 4 never does. Takes effect at the next elaboration.
    void setStopSeverity(std::uint8_t severity) noexcept { stopSeverity = severity; }

    /// The seed random draws start from (DESIGN_GUIDE.md section 3.8).
    void setSeed(std::uint32_t value) noexcept { seed = value; }

    // -----------------------------------------------------------------------
    // Analysis
    // -----------------------------------------------------------------------

    /// What one `analyse` call adds.
    struct AnalysisRequest {
        std::vector<std::filesystem::path> files;

        /// Sources analysed so that `files` resolve against them, whose units are **not**
        /// stored in the library (`--context`, section 8.2).
        ///
        /// slang type-checks from syntax, so a unit importing a package needs that package's
        /// source however the package was analysed before. Storing it here as well would put
        /// the same unit in two libraries, which an unqualified reference then cannot resolve.
        std::vector<std::filesystem::path> contextFiles;

        /// Sources belonging to the libraries this session resolves against.
        ///
        /// Read for the same reason as `contextFiles`, but a *specialisation* created from them
        /// is stored: a parameterisation the other library does not hold was created for this
        /// design, and nothing else would hold it. One the other library already has is
        /// excluded rather than duplicated (8.2).
        std::vector<std::filesystem::path> libraryFiles;
        std::vector<std::string> defines;
        std::vector<std::filesystem::path> includeDirectories;

        /// Top-level parameter overrides, each `NAME=VALUE`.
        std::vector<std::string> parameters;

        /// The top level the design will be elaborated from, when it is known.
        ///
        /// VHDL's adapter translates the hierarchy under its top rather than every unit analysed,
        /// so that a unit nothing instantiates cannot stop a design for using something the
        /// adapter lacks (DESIGN_GUIDE.md section 5.2); without one it translates the one entity
        /// that nothing instantiates, and refuses several. SystemVerilog's analysis does not use
        /// it.
        std::string top;

        /// Whether VHDL is translated as well as analysed. A library holds VHDL as NVC's
        /// analysed units rather than as EIR (section 4.4), so an analysis that runs nothing and
        /// names no top, `enzan analyse` without `--top`, has nothing to translate: which entity
        /// is the top is for a run to say.
        bool translateVhdl = true;

        /// The library VHDL sources are analysed into, by the name they refer to it by
        /// (`library neorv32;`): the output library's own name, or `work` when there is none and
        /// this is empty. SystemVerilog's analysis does not use it.
        std::string vhdlWork;

        /// Whether the IEEE packages' own warnings are reported, as GHDL's `--ieee-asserts`.
        bool ieeeWarnings = true;

        /// The revision of IEEE 1076 VHDL sources are analysed against: its year, or the
        /// year's last two digits.
        std::string vhdlStandard = "2008";
    };

    /// Analyses `request` together with everything analysed before it.
    ///
    /// Together, rather than incrementally, because analysis currently hands every source to
    /// one slang compilation. A design that is really analysed once per unit and cached is
    /// section 4.4's library store; until that exists, re-analysing is the behaviour that is
    /// *correct*, and the only thing wrong with it is that it is slower than it will be.
    ///
    /// When a library is open (`openLibrary`) and every source has a current entry in it, the
    /// units are loaded from there and the frontend does not run at all. Otherwise everything
    /// is analysed and every entry rewritten.
    bool analyse(const AnalysisRequest& request);

    /// True when a library this session resolves against holds analysed VHDL, from which a run
    /// given no sources translates its top (DESIGN_GUIDE.md section 8.2).
    [[nodiscard]] bool hasVhdlLibraries() const;

private:
    /// `analyse` for a design whose sources are VHDL, or that has none and libraries of VHDL.
    bool analyseVhdl(const AnalysisRequest& request);

    /// `analyse` for a design whose sources are of both languages (DESIGN_GUIDE.md section 18,
    /// M7): the VHDL translated under its top, each SystemVerilog module it instantiates
    /// specialised as it asks, and the two linked into one design.
    bool analyseMixed(const AnalysisRequest& request);

#ifdef ENZAN_WITH_VHDL
    /// Sets what of `options` is not the sources: the work library, the libraries analysed
    /// against, the standard, the top and the time unit. Without `sourcesGiven`, the work library
    /// is the input library whose VHDL declares the top. `keyOptions` receives what of them a
    /// library entry's key holds. False having reported when the work library is named otherwise
    /// than the library being written, or no input library declares the top.
    bool setVhdlOptions(const AnalysisRequest& request, bool sourcesGiven,
                        frontend::vhdl::DesignOptions& options, std::string& keyOptions);
#endif

    /// The input library whose VHDL declares `top`: the one `lib.top` or `work` names, or the one
    /// declaring it. Null having reported when none does, or several do.
    [[nodiscard]] const eir::Library* vhdlLibraryOf(std::string_view top,
                                                    std::string_view work) const;

    /// Asks the VHDL frontend for each resolution function `tree` needs and no translation has
    /// given yet, each for the number of drivers elaboration counted, links what comes back into
    /// the design, and records where each is (DESIGN_GUIDE.md section 3.7).
    bool answerResolutions(const elab::ElaboratedDesign& tree);

public:
    [[nodiscard]] bool hasDesign() const noexcept { return design != nullptr; }

    /// Every file the last `analyse` read: the sources, the files they included, and the
    /// context and library sources read for resolution.
    ///
    /// What `--depfile` writes (8.3). Sorted and free of duplicates.
    ///
    /// **Empty when the analysis was served from the library.** Nothing ran the frontend, so
    /// nothing knows what was included, and a caller MUST write no dependency file rather than
    /// one missing the headers: a prerequisite that quietly disappears is a rebuild that
    /// quietly stops happening.
    [[nodiscard]] std::span<const std::filesystem::path> filesRead() const noexcept {
        return lastFilesRead;
    }

    /// Takes `analysed` as this session's design, replacing whatever was there.
    ///
    /// The seam for a design that came from somewhere other than this session's own analysis:
    /// units loaded from a library, a design assembled from two frontends once mixed-language
    /// simulation arrives (section 3.5), and a test that builds EIR by hand to exercise the
    /// kernel without depending on a frontend.
    ///
    /// Everything elaborated from the previous design is torn down, because a session holding
    /// a kernel built from a design it no longer has would be a use-after-free waiting for the
    /// next command.
    void setDesign(std::unique_ptr<eir::Design> analysed);

    /// Opens `path` as the library analysis is written into, creating it when it is absent.
    ///
    /// Creating on absence is what `--out-lib <dir>` means: the library takes the directory's
    /// own name, and a name that is not an identifier is reported rather than adjusted (8.2).
    ///
    /// Opt-in. Section 4.4's store is a cache of what the frontend produced, and a cache that
    /// served units produced by a different translator would give a different simulation — so a
    /// user who suspects one has an obvious thing to stop doing. The format version and Enzan's
    /// version are both in every entry's key, which is the belt to that pair of braces.
    bool openOutputLibrary(const std::filesystem::path& path);

    /// Opens `path` as a library to resolve against, which MUST already exist.
    ///
    /// `name`, when given, overrides the logical name the library carries — for the cases a
    /// stored name cannot cover: two vendors each shipping a directory called `work`, or a
    /// path whose basename is not what the design refers to.
    bool addInputLibrary(const std::filesystem::path& path, std::string_view name = {});

    /// The sources every input library was analysed from, in the order the libraries were given.
    ///
    /// What `analyse --in-lib` reads as context: a unit instantiating one from another library
    /// needs that unit's declaration, because slang type-checks from syntax and EIR does not
    /// feed back into it (8.2). Reading the declaration is also what keeps a library
    /// parametrisable — the instantiation resolves its parameters here, rather than being
    /// limited to whatever parameterisation the other library happened to store.
    [[nodiscard]] std::vector<std::filesystem::path> inputLibrarySources() const;

    /// The include directories and macros those sources need, in the same order.
    ///
    /// A source is no use without them: its `` `include `` directives do not resolve, and a
    /// macro it was compiled with changes what it means rather than whether it parses.
    [[nodiscard]] std::vector<std::filesystem::path> inputLibraryIncludeDirectories() const;
    [[nodiscard]] std::vector<std::string> inputLibraryDefines() const;

    [[nodiscard]] bool hasLibrary() const noexcept { return library.isOpen(); }

    // -----------------------------------------------------------------------
    // VPI applications
    // -----------------------------------------------------------------------

    /// Loads a VPI application and runs its startup routines (DESIGN_GUIDE.md section 9.1).
    ///
    /// Called before analysis, so that a library that cannot be loaded is reported before a
    /// long analysis rather than after it, and so that its startup routines run before the
    /// design exists, as the standard orders it. Every elaboration from then on is shown to it.
    ///
    /// `commandLine` is what `vpi_get_vlog_info` reports, and is taken from the first call:
    /// one process has one command line however many applications it loads.
    bool loadVpiApplication(std::string_view specification,
                            std::span<const std::string> commandLine);

    /// Loads a VHPI application into the same context (section 9.7), which a design of both
    /// languages shows to both interfaces as one simulation. Only a build with the VHDL frontend
    /// has VHPI.
    bool loadVhpiApplication(std::string_view specification,
                             std::span<const std::string> commandLine);

    [[nodiscard]] bool hasVpiApplication() const noexcept;

    /// True when the last `analyse` came out of the library rather than the frontend.
    [[nodiscard]] bool reusedLibrary() const noexcept { return loadedFromLibrary; }

    // -----------------------------------------------------------------------
    // Elaboration
    // -----------------------------------------------------------------------

    /// Elaborates `topModule`, compiles it and prepares a kernel at time zero.
    ///
    /// An empty `topModule` lets elaboration choose, which it does only when the design has
    /// exactly one candidate. Calling this again replaces the whole simulation, which is what
    /// re-elaborating a running design means everywhere else.
    bool elaborate(std::string_view topModule);

    [[nodiscard]] bool isElaborated() const noexcept { return kernel != nullptr; }

    /// Counts how many times this session has thrown an elaboration away.
    ///
    /// What something borrowing the kernel compares to find out that what it holds has gone.
    /// **Pointer identity cannot answer this and must not be used for it.** Tearing an
    /// elaboration down frees the kernel and building the next one allocates the same sizes in
    /// the same order, so the allocator routinely hands the replacement the address the old one
    /// just released — on one machine and not the next, which is the worst way for it to be
    /// wrong. A borrower comparing addresses then keeps a pointer into a different simulation
    /// and no sanitiser can see it, because the memory is valid; it is simply someone else's.
    ///
    /// Monotonic and never reset, so a comparison cannot come out equal by wrapping.
    [[nodiscard]] std::uint64_t elaborationGeneration() const noexcept { return generation; }

    // -----------------------------------------------------------------------
    // Running
    // -----------------------------------------------------------------------

    /// Runs until `until`, or until the design stops when `until` is absent.
    ///
    /// `until` is an absolute time, not a duration: `run 100ns` is turned into an absolute
    /// limit by the command, because that is where the current time is known and where the
    /// unit conversion belongs.
    runtime::RunStatus run(std::optional<runtime::SimTime> until);

    /// Puts the design back to time zero, keeping the elaboration and the waveform files.
    ///
    /// Refused while a VPI application is loaded (section 9.5): it would still hold the state
    /// of the simulation being discarded.
    bool restart();

    [[nodiscard]] runtime::SimTime time() const noexcept;

    /// The status of the last `run`.
    [[nodiscard]] runtime::RunStatus lastStatus() const noexcept { return status; }

    /// True once `$finish` or `$fatal` has ended the simulation.
    [[nodiscard]] bool isFinished() const noexcept;

    /// The status `$finish` or `$fatal` asked for.
    [[nodiscard]] int exitStatus() const noexcept;

    // -----------------------------------------------------------------------
    // Inspection
    // -----------------------------------------------------------------------

    /// Returns the signal index for a hierarchical path, or `NoSignal`.
    [[nodiscard]] std::uint32_t findSignal(std::string_view path) const;

    static constexpr std::uint32_t NoSignal = 0xFFFF'FFFFU;

    /// Renders a signal's current value in `radix`.
    ///
    /// An enumerated signal whose value has a name is rendered by that name when `radix` is
    /// absent, matching what the wave window shows (section 10.3): a reader who asked what a
    /// state register holds wants `S_SEND`, and gets `3'h2` by asking for a radix. Anything else
    /// is rendered as it shows (section 3.6), a std_logic_vector as the bits its codes stand for,
    /// and a VHDL signal of a null range, which has no value, as nothing.
    [[nodiscard]] std::string examine(std::uint32_t signalIndex,
                                      std::optional<eir::Radix> radix) const;

    /// Parses `literal` against a signal's width and writes it once.
    ///
    /// Returns false having filled `error` when the literal does not parse, or is no value of
    /// the signal. The width is taken from the signal, so `deposit /tb/count 5` writes the right
    /// number of bits without the user counting them.
    bool deposit(std::uint32_t signalIndex, std::string_view literal, std::string& error);

    /// Writes `shown` once: a value as the signal shows it (section 3.6), which a VHDL signal
    /// stores as the codes its bits stand for, `1` a std_logic's '1'. Widened or narrowed to the
    /// width the signal shows at; false having filled `error` when it is no value of the signal,
    /// an unknown `bit` or anything of a null range.
    bool deposit(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error);

    /// Parses `literal` and holds the signal at it until `release`.
    bool force(std::uint32_t signalIndex, std::string_view literal, std::string& error);

    /// Holds the signal at `shown`, taken as `deposit` takes it, until `release`.
    bool force(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error);

    /// Releases a forced signal, returning false when it was not forced.
    bool release(std::uint32_t signalIndex);

    /// Returns every signal name matching a glob pattern, sorted by path.
    ///
    /// `*` matches any run of characters including `/`, and `?` matches one.
    ///
    /// Over names rather than over signals: a collapsed port gives one slot two hierarchical
    /// names, and a user asking about `/tb/u_dut/*` means the DUT's ports whether or not the
    /// storage for them lives in the parent.
    [[nodiscard]] std::vector<std::string> findSignals(std::string_view pattern) const;

    /// Returns the paths of every instance matching a glob pattern, in elaboration order.
    [[nodiscard]] std::vector<std::string> findInstances(std::string_view pattern) const;

    // -----------------------------------------------------------------------
    // Waveforms
    // -----------------------------------------------------------------------

    /// Adds `path` and what is inside it to what waveform files record.
    ///
    /// This is `log -r` and `add wave`: a selection is built up out of subtrees and individual
    /// signals, and a signal's own path is a scope holding exactly that signal (section 7.1).
    /// An empty path means the whole design and replaces everything selected so far.
    ///
    /// Refused once a file is open, because the signals a file declares are written into its
    /// header and cannot be added to afterwards.
    bool addWaveformScope(std::string_view path);

    [[nodiscard]] std::span<const std::string> waveformScopes() const noexcept { return scopes; }

    /// Records no signal wider than `bits` in a waveform file opened from now on, or one of any
    /// width when `bits` is 0, rather than the writer's default (section 7.1). Opening a file
    /// that leaves one out warns, naming it.
    void setWaveformWidthLimit(std::uint32_t bits) noexcept { widthLimit = bits; }

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
    /// Drops the kernel, the image and everything they were built from.
    void discardElaboration();

    /// Loads a VPI application, or a VHPI one when `vhpi` is set, creating the context the first
    /// time.
    bool loadApplication(std::string_view specification, std::span<const std::string> commandLine,
                         bool vhpi);

    /// Tells a loaded application the simulation is over, once (section 9.5).
    void endVpiSimulation();

    /// Parses `literal`, reporting through `error`.
    [[nodiscard]] static std::optional<eir::BitVector> parseValue(std::string_view literal,
                                                                  std::string& error);

    /// What `signalIndex` stores for `shown`, reporting through `error`.
    [[nodiscard]] std::optional<eir::BitVector>
    storedValue(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error) const;

    /// Pairs each analysed file with the buffer holding its text, for the library's keys.
    [[nodiscard]] static std::vector<eir::LibrarySource>
    librarySources(std::span<const std::filesystem::path> ordered,
                   std::span<const BufferId> buffers);

    /// Opens `writer` on `path` and attaches it to the kernel.
    bool addWriter(std::unique_ptr<wavedb::WaveformWriter> writer,
                   const std::filesystem::path& path);

    SourceManager& sources;
    DiagnosticEngine& diagnostics;
    std::ostream* transcript = nullptr;

    /// Every source given to `analyse`, in the order it was given.
    std::vector<std::filesystem::path> files;

    /// Sources read for resolution whose units this session does not store.
    std::vector<std::filesystem::path> contextFiles;

    /// Sources of the libraries this session resolves against.
    std::vector<std::filesystem::path> libraryFiles;

    /// Every file the last analysis read, for `--depfile`.
    std::vector<std::filesystem::path> lastFilesRead;
    std::vector<std::string> defines;
    std::vector<std::string> parameters;
    std::vector<std::filesystem::path> includeDirectories;

    eir::Library library;
    bool loadedFromLibrary = false;

    /// Libraries this session resolves against, in the order they were given.
    std::vector<eir::Library> inputLibraries;

    std::unique_ptr<eir::Design> design;
    std::unique_ptr<elab::ElaboratedDesign> elaborated;
    std::unique_ptr<codegen::CompiledDesign> compiled;
    std::unique_ptr<runtime::Image> image;
    std::unique_ptr<runtime::Kernel> kernel;

    /// The loaded VPI applications' view of the simulation, or null when none is loaded.
    ///
    /// Declared after the kernel so that it is destroyed first: it holds the kernel's hooks
    /// and must let go of them before the kernel goes.
    std::unique_ptr<pli::PliContext> vpi;

    /// Open waveform files. Owned here so that `quit` and the destructor both finish them.
    std::vector<std::unique_ptr<wavedb::WaveformWriter>> writers;

    /// What waveform files record, or empty for the whole design.
    std::vector<std::string> scopes;

    /// The widest signal a waveform file records, or nothing for the writer's default.
    std::optional<std::uint32_t> widthLimit;

    /// The run's unit, which every delay of the design counts.
    runtime::TimeScale scale;

    /// The unit a SystemVerilog design element with no time scale of its own is in, as `scale`
    /// was chosen; `scale` is finer where the design asks for a finer precision (section 3.1).
    runtime::TimeScale defaultScale;

    /// True when the unit was chosen, by `--timescale` or by a library holding units, rather than
    /// left at its default: a VHDL design then counts in it instead of in femtoseconds.
    bool scaleChosen = false;

    std::filesystem::path vhdlLibraries;

#ifdef ENZAN_WITH_VHDL
    /// What the design was analysed from when it was VHDL, which the frontend analyses again to
    /// translate the resolution functions elaboration asks for; null otherwise. Only a build
    /// with the VHDL frontend has its options to hold.
    std::unique_ptr<frontend::vhdl::DesignOptions> vhdlOptions;
#endif

    /// Where each VHDL resolution function translated for this design is, by its name and its
    /// number of drivers, which elaboration binds segments to.
    elab::ResolutionAnswers resolutionAnswers;
    runtime::RunStatus status = runtime::RunStatus::Quiescent;
    std::uint32_t deltaLimit = 10'000;
    std::uint8_t stopSeverity = 3;
    std::uint32_t seed = 0;

    /// Bumped by `discardElaboration`, which every path that replaces the simulation goes
    /// through. See `elaborationGeneration`.
    std::uint64_t generation = 0;
};

} // namespace enzan::session
