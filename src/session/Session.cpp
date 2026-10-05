// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/Session.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "elab/ElaboratedDesign.h"
#include "elab/Elaborator.h"
#include "pli/VpiContext.h"
#include "runtime/Image.h"
#include "wavedb/FstWriter.h"
#include "wavedb/VcdWriter.h"

#ifdef ENZAN_WITH_SYSTEMVERILOG
#include "frontend/sv/SystemVerilogFrontend.h"
#endif

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <utility>

namespace enzan::session {

namespace {

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

#ifdef ENZAN_WITH_SYSTEMVERILOG
    // Read here rather than by the frontend, so that each file has exactly one buffer: the
    // manager does not deduplicate, and a second buffer per file makes a location's identity
    // depend on which pass produced it.
    std::vector<std::filesystem::path> ordered;
    std::vector<BufferId> buffers;
    const std::size_t total = contextFiles.size() + libraryFiles.size() + files.size();
    ordered.reserve(total);
    buffers.reserve(total);

    // Context first, so that a package is in the compilation before whatever imports it.
    const auto read = [&](const std::filesystem::path& file) {
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
        library.setOptions(defines, includeDirectories, parameters);

        auto reused = std::make_unique<eir::Design>();
        if (library.load(stored, sources, *reused)) {
            design = std::move(reused);
            loadedFromLibrary = true;

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

    frontend::sv::Options options;
    options.filesRead = &lastFilesRead;
    options.files = ordered;
    options.buffers = buffers;
    options.defines = defines;
    options.includeDirectories = includeDirectories;
    options.timeUnitExponent = scale.precisionExponent();
    options.parameterOverrides = parameters;

    std::unique_ptr<eir::Design> analysed = frontend::sv::analyse(options, sources, diagnostics);
    if (analysed == nullptr) return false;

    design = std::move(analysed);

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

    // Before anything new is built, so that a failed elaboration on a running design leaves nothing
    // half-replaced for the next command to find.
    discardElaboration();

    elab::Elaborator elaborator(*design, diagnostics);
    std::unique_ptr<elab::ElaboratedDesign> tree = elaborator.elaborate(topModule);
    if (tree == nullptr) return false;

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
    if (vpi == nullptr) {
        vpi = std::make_unique<pli::VpiContext>(
            diagnostics, std::vector<std::string>(commandLine.begin(), commandLine.end()));
        vpi->setTranscript(transcript);
    }
    return vpi->load(specification);
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

    return eir::toString(value, radix.value_or(eir::Radix::Hexadecimal));
}

std::optional<eir::BitVector>
Session::parseValue(std::uint32_t signalIndex, std::string_view literal, std::string& error) const {
    std::optional<eir::BitVector> parsed = eir::BitVector::parse(literal);
    if (!parsed.has_value()) {
        error = "'" + std::string(literal) + "' is not a value Enzan understands";
        return std::nullopt;
    }

    // Widened or narrowed to the signal, so that `deposit /tb/count 5` writes the right number
    // of bits without the user counting them.
    const std::uint32_t width = image->signals()[signalIndex].bitWidth;
    if (parsed->width() == width) return parsed;

    eir::BitVector sized = eir::BitVector::filled(width, eir::Logic::Zero);
    eir::bitops::extend(sized.mutableRef(), parsed->ref(), false);
    return sized;
}

bool Session::deposit(std::uint32_t signalIndex, std::string_view literal, std::string& error) {
    const std::optional<eir::BitVector> value = parseValue(signalIndex, literal, error);
    if (!value.has_value()) return false;

    kernel->deposit(signalIndex, value->ref());
    return true;
}

bool Session::force(std::uint32_t signalIndex, std::string_view literal, std::string& error) {
    const std::optional<eir::BitVector> value = parseValue(signalIndex, literal, error);
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
