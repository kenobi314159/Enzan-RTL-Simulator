// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/Session.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Serialization.h"
#include "eir/Verifier.h"
#include "elab/ElaboratedDesign.h"
#include "elab/Elaborator.h"
#include "pli/PliContext.h"
#include "runtime/Image.h"
#include "wavedb/FstWriter.h"
#include "wavedb/VcdWriter.h"

#ifdef ENZAN_WITH_SYSTEMVERILOG
#include "frontend/sv/SystemVerilogFrontend.h"
#endif
#ifdef ENZAN_WITH_VHDL
#include "frontend/vhdl/VhdlFrontend.h"
#endif

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <utility>

namespace enzan::session {

namespace {

/// True when `file` is named as a VHDL source is: `.vhd` or `.vhdl`, in any case.
bool isVhdlSource(const std::filesystem::path& file) {
    std::string extension = file.extension().string();
    for (char& character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return extension == ".vhd" || extension == ".vhdl";
}

/// Matches `text` against a glob of `*` and `?`.
///
/// Iterative with a backtracking point rather than recursive, so that a pattern of many stars
/// against a long path cannot take exponential time — `find signals /*/*/*/*x` is a thing a
/// user types, and a `find` that appears to hang is worse than one that finds nothing.
bool globMatches(std::string_view pattern, std::string_view text) {
    std::size_t patternIndex = 0;
    std::size_t textIndex = 0;
    std::size_t starPattern = std::string_view::npos;
    std::size_t starText = 0;

    while (textIndex < text.size()) {
        if (patternIndex < pattern.size()
            && (pattern[patternIndex] == '?' || pattern[patternIndex] == text[textIndex])) {
            ++patternIndex;
            ++textIndex;
            continue;
        }

        if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            starPattern = patternIndex++;
            starText = textIndex;
            continue;
        }

        if (starPattern == std::string_view::npos) return false;

        patternIndex = starPattern + 1;
        textIndex = ++starText;
    }

    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') ++patternIndex;
    return patternIndex == pattern.size();
}

} // namespace

Session::Session(SourceManager& sourceManager, DiagnosticEngine& engine)
    : sources(sourceManager), diagnostics(engine) {}

Session::~Session() {
    // The application first, while the design it is told about still exists: an application
    // that writes its results when the simulation ends — cocotb does — reads the design as it
    // does so, and a batch run that stopped at its time limit ends here (section 9.5).
    endVpiSimulation();
    if (vpi != nullptr) vpi->detach();

    // Before the kernel goes, because a writer is registered as one of its listeners and
    // finishes its file from here.
    closeWaveforms();
}

void Session::setTranscript(std::ostream* stream) noexcept {
    transcript = stream;
    if (kernel != nullptr) kernel->setTranscript(stream);
    if (vpi != nullptr) vpi->setTranscript(stream);
}

std::ostream& Session::transcriptStream() const {
    return transcript != nullptr ? *transcript : std::cout;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

bool Session::analyse(const AnalysisRequest& request) {
    for (const std::filesystem::path& file : request.contextFiles) contextFiles.push_back(file);
    for (const std::filesystem::path& file : request.libraryFiles) libraryFiles.push_back(file);
    for (const std::filesystem::path& file : request.files) files.push_back(file);
    for (const std::string& define : request.defines) defines.push_back(define);
    for (const std::filesystem::path& directory : request.includeDirectories)
        includeDirectories.push_back(directory);
    for (const std::string& parameter : request.parameters) parameters.push_back(parameter);

    if (files.empty()) {
        diagnostics.report(DiagCode::NoInputFiles, SourceLocation());
        return false;
    }

    if (std::ranges::any_of(files, isVhdlSource)) return analyseVhdl(request);

#ifdef ENZAN_WITH_SYSTEMVERILOG
    // Read here rather than by the frontend, so that each file has exactly one buffer: the
    // manager does not deduplicate, and a second buffer per file makes a location's identity
    // depend on which pass produced it.
    std::vector<std::filesystem::path> ordered;
    std::vector<BufferId> buffers;
    const std::size_t total = contextFiles.size() + libraryFiles.size() + files.size();
    ordered.reserve(total);
    buffers.reserve(total);

    // Each file once, however many times it is named. Two input libraries built on one IP both
    // record its sources, and a file can be a source, a context file and a library's source
    // at once; slang refuses a second buffer at one path and keeps both at two spellings of
    // it, so a repeat is dropped here. Canonical, so that `./ip.sv` and `ip.sv` are one file,
    // and the first naming keeps its role: a context file stays unstored.
    std::set<std::filesystem::path> seen;

    // Context first, so that a package is in the compilation before whatever imports it.
    const auto read = [&](const std::filesystem::path& file) {
        std::error_code failure;
        std::filesystem::path key = std::filesystem::weakly_canonical(file, failure);
        if (failure) key = file;
        if (!seen.insert(std::move(key)).second) return true;

        const FileReadResult result = sources.readFile(file);
        if (!result.ok()) {
            reportFileReadFailure(diagnostics, file, result);
            return false;
        }
        ordered.push_back(file);
        buffers.push_back(result.buffer);
        return true;
    };
    for (const std::filesystem::path& file : contextFiles)
        if (!read(file)) return false;

    const std::size_t firstStored = ordered.size();
    for (const std::filesystem::path& file : libraryFiles)
        if (!read(file)) return false;

    for (const std::filesystem::path& file : files)
        if (!read(file)) return false;

    // This session's own sources, plus the sources of the libraries it resolves against: a
    // parameterisation those libraries do not hold was created *here*, for this design, and has
    // to be stored here or nothing holds it at all. Units an input library already has are
    // excluded below rather than duplicated (8.2).
    const std::vector<eir::LibrarySource> stored = librarySources(
        std::span(ordered).subspan(firstStored), std::span(buffers).subspan(firstStored));

    std::set<std::string, std::less<>> alreadyElsewhere;
    for (const eir::Library& input : inputLibraries) {
        for (const std::string& unit : input.unitNames()) alreadyElsewhere.insert(unit);
    }

    loadedFromLibrary = false;
    if (library.isOpen()) {
        library.setOptions(defines, includeDirectories, parameters, scale.precisionExponent());

        auto reused = std::make_unique<eir::Design>();
        if (library.load(stored, sources, *reused)) {
            design = std::move(reused);
            loadedFromLibrary = true;
#ifdef ENZAN_WITH_VHDL
            vhdlOptions.reset();
#endif
            resolutionAnswers.clear();

            // Left empty on purpose. A hit does not run the frontend, so nothing knows which
            // files were included, and a dependency file listing only the sources would drop
            // every header from the build's prerequisites — a rebuild that stops happening,
            // which is worse than one that happens twice. The caller writes no dependency file
            // rather than an incomplete one, and the file from the analysis that wrote the
            // entry stays correct because the entry is still current.
            lastFilesRead.clear();
            return true;
        }
    }

    // A design asking for a finer precision than the unit is run in that precision, unless
    // `--timescale` or a library fixed the unit (section 3.1). A library being written is one:
    // what it stores counts its unit, and a later analysis into it takes that unit as the one a
    // module with no time scale of its own is in, which has to be the one this analysis used.
    std::int8_t runTimeUnit = defaultScale.precisionExponent();
    frontend::sv::Options options;
    options.filesRead = &lastFilesRead;
    options.files = ordered;
    options.buffers = buffers;
    options.defines = defines;
    options.includeDirectories = includeDirectories;
    options.timeUnitExponent = defaultScale.precisionExponent();
    options.refineTimeUnit = !scaleChosen && !library.isOpen();
    options.runTimeUnit = &runTimeUnit;
    options.parameterOverrides = parameters;

    std::unique_ptr<eir::Design> analysed = frontend::sv::analyse(options, sources, diagnostics);
    if (analysed == nullptr) return false;

    scale = runtime::TimeScale(runTimeUnit);
    design = std::move(analysed);
#ifdef ENZAN_WITH_VHDL
    vhdlOptions.reset();
#endif
    resolutionAnswers.clear();

    // A failed store is reported and otherwise ignored: the design is in hand either way, and
    // refusing to simulate because a cache could not be written would be the cache making
    // things worse.
    if (library.isOpen()) {
        // Everything the frontend opened that is not one of the sources: the headers they
        // included, which is what a source's text alone cannot tell the library about.
        const std::set<std::filesystem::path> named(ordered.begin(), ordered.end());
        std::vector<std::filesystem::path> dependencies;
        for (const std::filesystem::path& file : lastFilesRead) {
            if (!named.contains(file)) dependencies.push_back(file);
        }

        static_cast<void>(
            library.store(stored, sources, *design, diagnostics, alreadyElsewhere, dependencies));
    }

    return true;
#else
    diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                       "analysis in a build configured with -DENZAN_ENABLE_SYSTEMVERILOG=OFF");
    return false;
#endif
}

bool Session::analyseVhdl(const AnalysisRequest& request) {
#ifdef ENZAN_WITH_VHDL
    // Every source is read first, so that one that cannot be is reported as that, whatever its
    // name says it holds, rather than as a design of two languages.
    frontend::vhdl::DesignOptions options;
    std::set<std::filesystem::path> seen;
    for (const std::filesystem::path& file : files) {
        std::error_code failure;
        std::filesystem::path key = std::filesystem::weakly_canonical(file, failure);
        if (failure) key = file;
        if (!seen.insert(std::move(key)).second) continue;

        const FileReadResult result = sources.readFile(file);
        if (!result.ok()) {
            reportFileReadFailure(diagnostics, file, result);
            return false;
        }
        options.units.sources.push_back(result.buffer);
    }
    if (!std::ranges::all_of(files, isVhdlSource) || !contextFiles.empty()
        || !libraryFiles.empty()) {
        diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                           "a design mixing VHDL and SystemVerilog sources");
        return false;
    }
    if (library.isOpen() || !inputLibraries.empty()) {
        diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                           "analysing VHDL into an Enzan library, or against one");
        return false;
    }
    if (!parameters.empty()) {
        diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                           "-G for a VHDL top level");
        return false;
    }

    // VHDL counts time in femtoseconds, its resolution limit, and a design is simulated in the
    // finest unit anything in it asks for (section 3.1), unless `--timescale` said otherwise.
    if (!scaleChosen) scale = runtime::TimeScale(runtime::FinestTimeExponent);

    options.units.work = frontend::vhdl::UnitLibrary{.name = request.vhdlWork, .directory = {}};
    options.units.searchDirectories.push_back(
        vhdlLibraries.empty() ? frontend::vhdl::standardLibraryDirectory() : vhdlLibraries);
    options.top = request.top;
    options.timeUnitExponent = scale.precisionExponent();
    options.ieeeWarnings = request.ieeeWarnings;
    const std::string_view year = request.vhdlStandard;
    if (year.ends_with("93")) options.units.standard = frontend::vhdl::Standard::Vhdl1993;
    if (year.ends_with("00")) options.units.standard = frontend::vhdl::Standard::Vhdl2000;
    if (year.ends_with("02")) options.units.standard = frontend::vhdl::Standard::Vhdl2002;
    if (year.ends_with("08")) options.units.standard = frontend::vhdl::Standard::Vhdl2008;
    if (year.ends_with("19")) options.units.standard = frontend::vhdl::Standard::Vhdl2019;

    std::unique_ptr<eir::Design> analysed = frontend::vhdl::analyse(options, sources, diagnostics);
    if (analysed == nullptr) return false;

    design = std::move(analysed);
    loadedFromLibrary = false;
    vhdlOptions = std::make_unique<frontend::vhdl::DesignOptions>(std::move(options));
    resolutionAnswers.clear();
    lastFilesRead.assign(files.begin(), files.end());
    std::ranges::sort(lastFilesRead);
    lastFilesRead.erase(std::ranges::unique(lastFilesRead).begin(), lastFilesRead.end());
    return true;
#else
    static_cast<void>(request);
    diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                       "VHDL analysis in a build configured with -DENZAN_ENABLE_VHDL=OFF");
    return false;
#endif
}

bool Session::chooseTimeScale(std::optional<runtime::TimeScale> requested) {
    // Every open library that holds units, with its unit: the output library as well as the
    // inputs, since analysing into a library adds to what it holds.
    std::vector<std::pair<const eir::Library*, runtime::TimeScale>> holding;
    const auto consider = [&holding](const eir::Library& candidate) {
        if (!candidate.isOpen()) return;
        if (const std::optional<std::int8_t> unit = candidate.timeUnit())
            holding.emplace_back(&candidate, runtime::TimeScale(*unit));
    };
    consider(library);
    for (const eir::Library& input : inputLibraries) consider(input);

    for (const auto& [other, unit] : holding) {
        const auto& [first, firstUnit] = holding.front();
        if (unit.precisionExponent() != firstUnit.precisionExponent()) {
            diagnostics.report(DiagCode::LibraryTimeUnitsDiffer, SourceLocation(), first->name(),
                               firstUnit.unitName(), other->name(), unit.unitName());
            return false;
        }
    }

    if (holding.empty()) {
        scale = requested.value_or(runtime::TimeScale());
        defaultScale = scale;
        scaleChosen = requested.has_value();
        return true;
    }

    const auto& [first, unit] = holding.front();
    if (requested.has_value() && requested->precisionExponent() != unit.precisionExponent()) {
        diagnostics.report(DiagCode::TimeScaleConflictsWithLibrary, SourceLocation(),
                           requested->unitName(), first->name(), unit.unitName(),
                           requested->unitName());
        return false;
    }
    scale = unit;
    defaultScale = unit;
    scaleChosen = true;
    return true;
}

bool Session::openOutputLibrary(const std::filesystem::path& path) {
    std::error_code failure;
    if (std::filesystem::exists(path / "enzan.lib", failure))
        return library.open(path, diagnostics);

    return library.create(path, {}, diagnostics);
}

std::vector<eir::LibrarySource>
Session::librarySources(std::span<const std::filesystem::path> ordered,
                        std::span<const BufferId> buffers) {
    std::vector<eir::LibrarySource> stored;
    stored.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size() && index < buffers.size(); ++index)
        stored.push_back({.path = ordered[index], .buffer = buffers[index]});
    return stored;
}

// ---------------------------------------------------------------------------
// Elaboration
// ---------------------------------------------------------------------------

void Session::setDesign(std::unique_ptr<eir::Design> analysed) {
    discardElaboration();
    design = std::move(analysed);
    loadedFromLibrary = false;
#ifdef ENZAN_WITH_VHDL
    vhdlOptions.reset();
#endif
    resolutionAnswers.clear();
}

bool Session::answerResolutions(const elab::ElaboratedDesign& tree) {
    std::vector<std::uint32_t> asked;
    for (std::uint32_t index = 0; index < tree.resolutionCalls().size(); ++index) {
        if (!tree.resolutionCalls()[index].answered()) asked.push_back(index);
    }
    if (asked.empty()) return true;
#ifdef ENZAN_WITH_VHDL
    if (vhdlOptions == nullptr) {
        diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                           "a VHDL resolved signal in a design not analysed from its sources here");
        return false;
    }
    std::vector<frontend::vhdl::ResolutionRequest> requests;
    requests.reserve(asked.size());
    for (const std::uint32_t index : asked) {
        const elab::ResolutionCall& call = tree.resolutionCalls()[index];
        requests.push_back(
            frontend::vhdl::ResolutionRequest{.function = call.function, .drivers = call.drivers});
    }
    std::optional<frontend::vhdl::ResolvedFunctions> found =
        frontend::vhdl::resolutions(*vhdlOptions, sources, requests, diagnostics);
    if (!found) return false;

    // A package the design already has gains the functions it lacks; a specialisation is named
    // for what it is, so one it has is the same one (section 3.7).
    for (eir::ModuleDefinition& definition : found->definitions) {
        const std::string qualified = definition.library.empty()
                                          ? definition.name
                                          : definition.library + "." + definition.name;
        if (const eir::ModuleDefinition* existing = design->findModule(qualified)) {
            eir::ModuleDefinition merged = *existing;
            for (eir::Subprogram& subprogram : definition.subprograms) {
                const bool known = std::ranges::any_of(merged.subprograms,
                                                       [&subprogram](const eir::Subprogram& held) {
                                                           return held.name == subprogram.name;
                                                       });
                if (!known) merged.subprograms.push_back(std::move(subprogram));
            }
            design->addModule(std::move(merged));
        } else {
            design->addModule(std::move(definition));
        }
    }
    if (!eir::verify(*design, diagnostics)) return false;
    for (std::size_t index = 0; index < asked.size(); ++index) {
        resolutionAnswers.insert_or_assign(
            std::pair(requests[index].function, requests[index].drivers),
            std::pair(found->answers[index].definition, found->answers[index].subprogram));
    }
    return true;
#else
    diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                       "a VHDL resolved signal in a build without the VHDL frontend");
    return false;
#endif
}

void Session::discardElaboration() {
    // Counted before anything is freed, and counted even when there was nothing to free: a
    // borrower is entitled to notice that the answer to "is this still yours" changed, and an
    // over-cautious renewal costs a round trip where a missed one costs a pointer into a
    // simulation that no longer exists.
    ++generation;

    // An application holds handles into the design and the kernel's hooks; it lets go of both
    // before either is freed, having first been told the simulation it watched has ended.
    if (vpi != nullptr && kernel != nullptr) {
        endVpiSimulation();
        vpi->detach();
    }

    // Torn down in dependency order, so that nothing is left holding a reference to something
    // already gone: the writers listen to the kernel, the kernel reads the image, and the
    // image reads the elaboration and the compiled bodies.
    closeWaveforms();
    kernel.reset();
    image.reset();
    compiled.reset();
    elaborated.reset();
}

bool Session::addInputLibrary(const std::filesystem::path& path, std::string_view name) {
    eir::Library opened;
    if (!opened.open(path, diagnostics)) return false;
    if (!name.empty() && !opened.overrideName(name)) {
        diagnostics.report(DiagCode::LibraryBadName, SourceLocation(), std::string(name));
        return false;
    }

    inputLibraries.push_back(std::move(opened));
    return true;
}

std::vector<std::filesystem::path> Session::inputLibrarySources() const {
    std::vector<std::filesystem::path> all;
    for (const eir::Library& input : inputLibraries) {
        for (const std::filesystem::path& source : input.sources()) all.push_back(source);
    }
    return all;
}

std::vector<std::filesystem::path> Session::inputLibraryIncludeDirectories() const {
    std::vector<std::filesystem::path> all;
    for (const eir::Library& input : inputLibraries) {
        for (const std::filesystem::path& directory : input.includeDirectories())
            all.push_back(directory);
    }
    return all;
}

std::vector<std::string> Session::inputLibraryDefines() const {
    std::vector<std::string> all;
    for (const eir::Library& input : inputLibraries) {
        for (const std::string& define : input.defines()) all.push_back(define);
    }
    return all;
}

bool Session::elaborate(std::string_view topModule) {
    // A run may be given nothing but libraries to resolve against, so the design to elaborate
    // is whatever was analysed here plus every unit the input libraries hold. Merged in the
    // order the libraries were given, which is the order 8.2 says they are searched and the
    // order an ambiguity is reported in.
    if (design == nullptr && !inputLibraries.empty()) design = std::make_unique<eir::Design>();

    for (const eir::Library& input : inputLibraries) {
        if (!input.loadAll(*design, diagnostics)) return false;
    }

    if (design == nullptr) {
        diagnostics.report(DiagCode::NothingAnalysed, SourceLocation());
        return false;
    }

    // Two libraries built on one IP each hold the parameterisations they needed, and a design
    // analysed here against a library reproduces the units it reads from that library's
    // sources. Copies that are the same unit are one; any that differ stay ambiguous (8.2).
    design->mergeIdenticalDefinitions([this](const eir::ModuleDefinition& definition) {
        return eir::encodeContent(definition, sources);
    });

    // Before anything new is built, so that a failed elaboration on a running design leaves nothing
    // half-replaced for the next command to find.
    discardElaboration();

    elab::Elaborator elaborator(*design, diagnostics);
    std::unique_ptr<elab::ElaboratedDesign> tree =
        elaborator.elaborate(topModule, resolutionAnswers);
    if (tree == nullptr) return false;

    // A resolved signal's function for a driver count no translation has given yet is asked for,
    // and the design elaborated again with it (DESIGN_GUIDE.md section 3.7).
    const bool unanswered = std::ranges::any_of(
        tree->resolutionCalls(), [](const elab::ResolutionCall& call) { return !call.answered(); });
    if (unanswered) {
        if (!answerResolutions(*tree)) return false;
        tree = elab::Elaborator(*design, diagnostics).elaborate(topModule, resolutionAnswers);
        if (tree == nullptr) return false;
    }

    codegen::Compiler compiler(diagnostics);
    std::unique_ptr<codegen::CompiledDesign> bodies = compiler.compile(*design);
    if (bodies == nullptr) return false;

    elaborated = std::move(tree);
    compiled = std::move(bodies);
    image = std::make_unique<runtime::Image>(*design, *elaborated, *compiled);

    runtime::KernelOptions options;
    options.timeScale = scale;
    options.deltaLimit = deltaLimit;
    options.transcript = transcript;
    options.seed = seed;
    options.stopSeverity = stopSeverity;

    kernel = std::make_unique<runtime::Kernel>(*image, diagnostics, options);
    kernel->initialize();
    status = runtime::RunStatus::TimeLimitReached;

    // Everything an application does to the design starts here (section 9.1).
    if (vpi != nullptr) {
        vpi->attach(*image, *kernel, &sources);
        vpi->startOfSimulation();
    }
    return true;
}

bool Session::loadVpiApplication(std::string_view specification,
                                 std::span<const std::string> commandLine) {
    return loadApplication(specification, commandLine, false);
}

bool Session::loadVhpiApplication(std::string_view specification,
                                  std::span<const std::string> commandLine) {
    return loadApplication(specification, commandLine, true);
}

bool Session::loadApplication(std::string_view specification,
                              std::span<const std::string> commandLine, bool vhpi) {
    if (vpi == nullptr) {
        vpi = std::make_unique<pli::PliContext>(
            diagnostics, std::vector<std::string>(commandLine.begin(), commandLine.end()));
        vpi->setTranscript(transcript);
    }
    return vpi->load(specification,
                     vhpi ? pli::PliContext::Interface::Vhpi : pli::PliContext::Interface::Vpi);
}

bool Session::hasVpiApplication() const noexcept {
    return vpi != nullptr && vpi->hasApplications();
}

void Session::endVpiSimulation() {
    if (vpi != nullptr) vpi->endOfSimulation();
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

runtime::RunStatus Session::run(std::optional<runtime::SimTime> until) {
    if (kernel == nullptr) {
        diagnostics.report(DiagCode::NoSimulation, SourceLocation(), "run");
        return runtime::RunStatus::Error;
    }

    status = kernel->run(until.value_or(runtime::NoTime));

    // A run that stopped at a time limit or was interrupted can continue; one that finished,
    // ran out of events or failed cannot, and an application is told so now rather than when
    // the process happens to exit.
    if (status == runtime::RunStatus::Finished || status == runtime::RunStatus::Quiescent
        || status == runtime::RunStatus::Error) {
        endVpiSimulation();
    }

    // The files are brought up to date every time the run stops, because a user who paused to
    // look at a waveform expects the file to contain what has happened rather than everything
    // up to the last time advance.
    for (const std::unique_ptr<wavedb::WaveformWriter>& writer : writers) writer->flush();

    return status;
}

bool Session::restart() {
    if (kernel == nullptr) {
        diagnostics.report(DiagCode::NoSimulation, SourceLocation(), "restart");
        return false;
    }

    if (hasVpiApplication()) {
        diagnostics.report(DiagCode::RestartWithVpiApplication, SourceLocation());
        return false;
    }

    kernel->initialize();
    status = runtime::RunStatus::TimeLimitReached;
    return true;
}

runtime::SimTime Session::time() const noexcept {
    return kernel != nullptr ? kernel->time() : 0;
}

bool Session::isFinished() const noexcept {
    return status == runtime::RunStatus::Finished || status == runtime::RunStatus::Error;
}

int Session::exitStatus() const noexcept {
    return kernel != nullptr ? kernel->exitStatus() : 0;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

std::uint32_t Session::findSignal(std::string_view path) const {
    if (image == nullptr) return NoSignal;

    const std::uint32_t found = image->findSignal(path);
    return found == runtime::Image::NoSignal ? NoSignal : found;
}

std::string Session::examine(std::uint32_t signalIndex, std::optional<eir::Radix> radix) const {
    const runtime::Signal& signal = image->signals()[signalIndex];
    const eir::ConstBitRef value = kernel->arena().at(signal.wordOffset, signal.bitWidth);

    if (!radix.has_value()) {
        // An enumerated signal reads by the name its type gives the value, which is what the
        // wave window shows and what a reader asked about a state register wants.
        const std::optional<std::uint32_t> slotIndex = elaborated->findSlot(signal.path);
        if (slotIndex.has_value()) {
            const elab::Slot& slot = elaborated->slots()[*slotIndex];
            if (slot.enumIndex != elab::Slot::NoEnum) {
                const eir::EnumDefinition& enumeration = elaborated->enumerations()[slot.enumIndex];
                for (const eir::EnumMember& member : enumeration.members)
                    if (member.value.ref().identicalTo(value)) return member.name;
            }
        }
    }

    // As every waveform shows it: a VHDL signal's codes as the bits they stand for, and one of a
    // null range, whose one bit is a placeholder, as nothing.
    if (elaborated->slots()[signalIndex].holdsNothing) return {};
    if (signal.encoding == eir::Encoding::Bits)
        return eir::toString(value, radix.value_or(eir::Radix::Hexadecimal));
    eir::BitVector shown(eir::presentedWidth(signal.encoding, signal.bitWidth));
    eir::present(signal.encoding, value, shown.mutableRef());
    return eir::toString(shown.ref(), radix.value_or(eir::Radix::Hexadecimal));
}

std::optional<eir::BitVector> Session::parseValue(std::string_view literal, std::string& error) {
    std::optional<eir::BitVector> parsed = eir::BitVector::parse(literal);
    if (!parsed.has_value())
        error = "'" + std::string(literal) + "' is not a value Enzan understands";
    return parsed;
}

std::optional<eir::BitVector>
Session::storedValue(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error) const {
    const runtime::Signal& signal = image->signals()[signalIndex];
    if (elaborated->slots()[signalIndex].holdsNothing) {
        error = signal.path + " is a null array, so it has no bits to write";
        return std::nullopt;
    }

    // Widened or narrowed to the signal as it shows, so that `deposit /tb/count 5` writes the
    // right number of bits without the user counting them, and a std_logic takes the one its
    // code stands for.
    eir::BitVector sized = eir::BitVector::filled(
        eir::presentedWidth(signal.encoding, signal.bitWidth), eir::Logic::Zero);
    eir::bitops::extend(sized.mutableRef(), shown, false);
    eir::BitVector stored = eir::BitVector::filled(signal.bitWidth, eir::Logic::Zero);
    if (!eir::storeShown(signal.encoding, sized.ref(), stored.mutableRef())) {
        error = "'" + eir::toString(sized.ref(), eir::Radix::Binary) + "' is no value of "
                + signal.path + ", whose bits are each 0 or 1";
        return std::nullopt;
    }
    return stored;
}

bool Session::deposit(std::uint32_t signalIndex, std::string_view literal, std::string& error) {
    const std::optional<eir::BitVector> value = parseValue(literal, error);
    return value.has_value() && deposit(signalIndex, value->ref(), error);
}

bool Session::deposit(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error) {
    const std::optional<eir::BitVector> value = storedValue(signalIndex, shown, error);
    if (!value.has_value()) return false;

    kernel->deposit(signalIndex, value->ref());
    return true;
}

bool Session::force(std::uint32_t signalIndex, std::string_view literal, std::string& error) {
    const std::optional<eir::BitVector> value = parseValue(literal, error);
    return value.has_value() && force(signalIndex, value->ref(), error);
}

bool Session::force(std::uint32_t signalIndex, eir::ConstBitRef shown, std::string& error) {
    const std::optional<eir::BitVector> value = storedValue(signalIndex, shown, error);
    if (!value.has_value()) return false;

    kernel->force(signalIndex, value->ref());
    return true;
}

bool Session::release(std::uint32_t signalIndex) {
    return kernel->release(signalIndex);
}

std::vector<std::string> Session::findSignals(std::string_view pattern) const {
    std::vector<std::string> found;
    if (elaborated == nullptr) return found;

    // Over every *name*, not over the signals: a collapsed port gives one slot two
    // hierarchical names, and `find signals /tb/u_dut/*` asking about the DUT's ports must
    // find them under the DUT even though the storage lives in the parent.
    for (const auto& [path, slotIndex] : elaborated->namesByPath())
        if (globMatches(pattern, path)) found.push_back(path);
    return found;
}

std::vector<std::string> Session::findInstances(std::string_view pattern) const {
    std::vector<std::string> found;
    if (elaborated == nullptr) return found;

    for (const elab::Instance& instance : elaborated->instances())
        if (globMatches(pattern, instance.path)) found.push_back(instance.path);
    return found;
}

// ---------------------------------------------------------------------------
// Waveforms
// ---------------------------------------------------------------------------

bool Session::addWaveformScope(std::string_view path) {
    if (!writers.empty()) {
        diagnostics.report(DiagCode::WaveformScopeAfterOpen, SourceLocation());
        return false;
    }

    if (path.empty()) {
        scopes.clear();
        return true;
    }

    if (std::ranges::find(scopes, path) == scopes.end()) scopes.emplace_back(path);
    return true;
}

bool Session::addWriter(std::unique_ptr<wavedb::WaveformWriter> writer,
                        const std::filesystem::path& path) {
    if (kernel == nullptr) {
        diagnostics.report(DiagCode::NoSimulation, SourceLocation(), "a waveform file");
        return false;
    }

    for (const std::string& selected : scopes) writer->addScope(selected);

    std::string error;
    if (!writer->open(path, error)) {
        diagnostics.report(DiagCode::CannotOpenFile, SourceLocation(), path.string(), error);
        return false;
    }

    kernel->addListener(*writer);
    writers.push_back(std::move(writer));
    return true;
}

bool Session::openVcd(const std::filesystem::path& path) {
    if (kernel == nullptr) {
        diagnostics.report(DiagCode::NoSimulation, SourceLocation(), "vcd file");
        return false;
    }

    return addWriter(std::make_unique<wavedb::VcdWriter>(*image, kernel->arena(), scale), path);
}

bool Session::openFst(const std::filesystem::path& path) {
    if (kernel == nullptr) {
        diagnostics.report(DiagCode::NoSimulation, SourceLocation(), "fst file");
        return false;
    }

    return addWriter(std::make_unique<wavedb::FstWriter>(*image, kernel->arena(), scale), path);
}

void Session::closeWaveforms() {
    for (const std::unique_ptr<wavedb::WaveformWriter>& writer : writers) {
        writer->close();
        if (kernel != nullptr) kernel->removeListener(*writer);
    }
    writers.clear();
}

} // namespace enzan::session
