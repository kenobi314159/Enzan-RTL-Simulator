// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/Session.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "control/CompileOrder.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "elab/ElaboratedDesign.h"
#include "elab/Elaborator.h"
#include "runtime/Image.h"
#include "wavedb/FstWriter.h"
#include "wavedb/VcdWriter.h"

#ifdef ENZAN_WITH_SYSTEMVERILOG
#include "frontend/sv/SystemVerilogFrontend.h"
#endif

#include <algorithm>
#include <iostream>
#include <map>
#include <utility>

namespace enzan::control {

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
    // Before the kernel goes, because a writer is registered as one of its listeners and
    // finishes its file from here.
    closeWaveforms();
}

void Session::setTranscript(std::ostream* stream) noexcept {
    transcript = stream;
    if (kernel != nullptr) kernel->setTranscript(stream);
}

std::ostream& Session::transcriptStream() const {
    return transcript != nullptr ? *transcript : std::cout;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

bool Session::analyse(const AnalysisRequest& request) {
    for (const std::filesystem::path& file : request.files) files.push_back(file);
    for (const std::string& define : request.defines) defines.push_back(define);
    for (const std::filesystem::path& directory : request.includeDirectories)
        includeDirectories.push_back(directory);

    if (files.empty()) {
        diagnostics.report(DiagCode::NoInputFiles, SourceLocation());
        return false;
    }

#ifdef ENZAN_WITH_SYSTEMVERILOG
    std::vector<std::filesystem::path> ordered;
    std::vector<BufferId> buffers;
    if (!inferCompileOrder(files, sources, diagnostics, ordered, &buffers)) return false;

    // The pre-scan has already read and buffered every file. Those buffers go on to both the
    // library and the frontend, so each file is read once and has exactly one buffer — which
    // is what lets anything downstream compare a location's buffer and mean it.
    const std::vector<eir::LibrarySource> stored = librarySources(ordered, buffers);

    loadedFromLibrary = false;
    if (library.isOpen()) {
        library.setOptions(defines, includeDirectories);

        auto reused = std::make_unique<eir::Design>();
        if (library.load(stored, sources, *reused)) {
            design = std::move(reused);
            loadedFromLibrary = true;
            return true;
        }
    }

    frontend::sv::Options options;
    options.files = ordered;
    options.buffers = buffers;
    options.defines = defines;
    options.includeDirectories = includeDirectories;

    std::unique_ptr<eir::Design> analysed = frontend::sv::analyse(options, sources, diagnostics);
    if (analysed == nullptr) return false;

    design = std::move(analysed);

    // A failed store is reported and otherwise ignored: the design is in hand either way, and
    // refusing to simulate because a cache could not be written would be the cache making
    // things worse.
    if (library.isOpen()) static_cast<void>(library.store(stored, sources, *design, diagnostics));

    return true;
#else
    diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                       "analysis in a build configured with -DENZAN_ENABLE_SYSTEMVERILOG=OFF");
    return false;
#endif
}

bool Session::openLibrary(const std::filesystem::path& path) {
    return library.open(path, diagnostics);
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

    // Torn down in dependency order, so that nothing is left holding a reference to something
    // already gone: the writers listen to the kernel, the kernel reads the image, and the
    // image reads the elaboration and the compiled bodies.
    closeWaveforms();
    kernel.reset();
    image.reset();
    compiled.reset();
    elaborated.reset();
}

bool Session::elaborate(std::string_view topModule) {
    if (design == nullptr) {
        diagnostics.report(DiagCode::NothingAnalysed, SourceLocation());
        return false;
    }

    // Before anything new is built, so that a failed `vsim` on a running design leaves nothing
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
    return true;
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

    kernel->initialize();
    status = runtime::RunStatus::TimeLimitReached;
    return true;
}

bool Session::serveTo(const std::filesystem::path& socketPath, std::string& error) {
    if (server == nullptr) {
        error = "this kernel cannot serve a front end";
        return false;
    }
    if (kernel == nullptr) {
        error = "there is no simulation to serve; `vsim` first";
        return false;
    }

    // Refused rather than done badly. Serving puts the design back to time zero and starts
    // recording there, because that is what makes the values the wave window shows at time zero
    // the ones the design begins from. A session that has already run would therefore be
    // silently reset, losing exactly the run the user wanted to look at — so say so, and say
    // where the command goes instead.
    if (time() != 0) {
        error = "this design has already run to " + scale.format(time())
                + "; serving it would put it back to time zero and lose that run. Put `listen` "
                  "before the run and let the front end drive it, or `restart` first.";
        return false;
    }

    return server->serve(socketPath, error);
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
// Source debugging
// ---------------------------------------------------------------------------

BufferId Session::findSourceBuffer(std::string_view file, std::string& error) const {
    std::string ambiguity;
    const BufferId buffer = findBufferByName(sources, file, &ambiguity);
    if (buffer.valid()) return buffer;

    error = ambiguity.empty() ? "no analysed source is called '" + std::string(file) + "'"
                              : "'" + std::string(file) + "' matches " + ambiguity;
    return {};
}

std::uint32_t Session::addBreakpoint(std::string_view file, std::uint32_t line,
                                     std::string& error) {
    if (kernel == nullptr) {
        error = "`bp` needs a simulation; run `vsim <top>` first";
        return 0;
    }

    const BufferId buffer = findSourceBuffer(file, error);
    if (!buffer.valid()) return 0;

    const std::uint32_t id = kernel->addBreakpoint(buffer, line);
    if (id == 0) {
        error = std::string(file) + ":" + std::to_string(line)
                + " compiled to no instructions, so nothing could ever stop there";
    }
    return id;
}

bool Session::removeBreakpoint(std::uint32_t id) {
    return kernel != nullptr && kernel->removeBreakpoint(id);
}

bool Session::setBreakpointEnabled(std::uint32_t id, bool enabled) {
    return kernel != nullptr && kernel->setBreakpointEnabled(id, enabled);
}

void Session::clearBreakpoints() {
    if (kernel != nullptr) kernel->clearBreakpoints();
}

std::span<const runtime::Breakpoint> Session::breakpoints() const {
    if (kernel == nullptr) return {};
    return kernel->breakpoints();
}

void Session::step() {
    if (kernel != nullptr) kernel->requestStep();
}

std::string Session::describe(const runtime::Breakpoint& breakpoint) const {
    std::string text = std::to_string(breakpoint.id) + " "
                       + std::string(sources.bufferName(breakpoint.buffer)) + ":"
                       + std::to_string(breakpoint.line);
    if (!breakpoint.enabled) text += " (disabled)";
    return text;
}

std::string Session::stopDescription() const {
    if (kernel == nullptr) return {};

    const runtime::StopPoint& stop = kernel->stopPoint();
    if (!stop.valid()) return {};

    std::string text;
    if (stop.location.valid()) {
        text += sources.bufferName(stop.location.buffer());
        text += ":";
        text += std::to_string(sources.lineNumber(stop.location));
    } else {
        text += "an unattributed instruction";
    }

    if (stop.processIndex < image->processes().size()) {
        text += " in ";
        text += image->processes()[stop.processIndex].name;
    }
    return text;
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

} // namespace enzan::control
