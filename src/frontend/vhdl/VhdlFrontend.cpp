// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/VhdlFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Serialization.h"
#include "eir/Verifier.h"
#include "frontend/vhdl/NvcAnalysis.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace enzan::frontend::vhdl {

namespace {

// The child reports to its parent through a pipe, as a stream of records. Both ends are one
// binary in two processes, so the encoding is whatever is simplest to read back: fixed-width
// integers in the machine's own order, and text as a length followed by its bytes.

/// What a record is, in its first byte.
enum class Record : std::uint8_t {
    /// One of NVC's diagnostics, translated.
    Diagnostic = 1,
    /// The analysis ran to its end, followed by its error count. A stream without one is a child
    /// that NVC ended part-way.
    Finished = 2,
    /// The translated design, as `eir::encodeModules` wrote it, then how many definitions it holds
    /// and the library of each, which that encoding leaves out.
    Design = 3,
    /// Where each requested resolution function is: how many, then the definition and the
    /// subprogram of each.
    Resolutions = 4,
};

class Encoder {
public:
    void byte(std::uint8_t value) { bytes.push_back(static_cast<char>(value)); }

    void u32(std::uint32_t value) {
        std::array<char, sizeof value> raw{};
        std::memcpy(raw.data(), &value, sizeof value);
        bytes.append(raw.data(), raw.size());
    }

    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.append(value);
    }

    /// The buffer's index plus one, so that zero is no buffer, then the offset.
    void location(SourceLocation value) {
        u32(value.valid() ? value.buffer().index() + 1U : 0U);
        u32(value.offset());
    }

    [[nodiscard]] std::string_view view() const noexcept { return bytes; }

private:
    std::string bytes;
};

class Decoder {
public:
    explicit Decoder(std::string_view bytes) noexcept : rest(bytes) {}

    [[nodiscard]] bool empty() const noexcept { return rest.empty(); }

    std::optional<std::uint8_t> byte() {
        if (rest.empty()) return std::nullopt;
        const auto value = static_cast<std::uint8_t>(rest.front());
        rest.remove_prefix(1);
        return value;
    }

    std::optional<std::uint32_t> u32() {
        std::uint32_t value = 0;
        if (rest.size() < sizeof value) return std::nullopt;
        std::memcpy(&value, rest.data(), sizeof value);
        rest.remove_prefix(sizeof value);
        return value;
    }

    std::optional<std::string> text() {
        const std::optional<std::uint32_t> length = u32();
        if (!length || rest.size() < *length) return std::nullopt;
        std::string value(rest.substr(0, *length));
        rest.remove_prefix(*length);
        return value;
    }

    std::optional<SourceLocation> location() {
        const std::optional<std::uint32_t> buffer = u32();
        const std::optional<std::uint32_t> offset = u32();
        if (!buffer || !offset) return std::nullopt;
        if (*buffer == 0) return SourceLocation();
        return SourceLocation(BufferId::fromIndex(*buffer - 1U), *offset);
    }

private:
    std::string_view rest;
};

/// Writes all of `bytes`, however many writes that takes.
bool writeAll(int descriptor, std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

/// The child's end: each diagnostic is written to the parent as NVC reports it, so that one NVC
/// ends the process after still reaches the parent.
class PipeSink final : public DiagnosticSink {
public:
    explicit PipeSink(int descriptor) noexcept : output(descriptor) {}

    void deliver(const Diagnostic& diagnostic) override {
        Encoder encoder;
        encoder.byte(static_cast<std::uint8_t>(Record::Diagnostic));
        encoder.u32(static_cast<std::uint32_t>(diagnostic.code));
        encoder.location(diagnostic.location);
        encoder.text(diagnostic.message);
        encoder.u32(static_cast<std::uint32_t>(diagnostic.notes.size()));
        for (const DiagnosticNote& note : diagnostic.notes) {
            encoder.location(note.location);
            encoder.text(note.message);
        }
        static_cast<void>(writeAll(output, encoder.view()));
    }

    void design(std::string_view encoded, const std::vector<std::string>& libraries) const {
        Encoder encoder;
        encoder.byte(static_cast<std::uint8_t>(Record::Design));
        encoder.text(encoded);
        encoder.u32(static_cast<std::uint32_t>(libraries.size()));
        for (const std::string& library : libraries) encoder.text(library);
        static_cast<void>(writeAll(output, encoder.view()));
    }

    void answers(const std::vector<ResolutionAnswer>& found) const {
        Encoder encoder;
        encoder.byte(static_cast<std::uint8_t>(Record::Resolutions));
        encoder.u32(static_cast<std::uint32_t>(found.size()));
        for (const ResolutionAnswer& answer : found) {
            encoder.text(answer.definition);
            encoder.text(answer.subprogram);
        }
        static_cast<void>(writeAll(output, encoder.view()));
    }

    void finish(unsigned errors) const {
        Encoder encoder;
        encoder.byte(static_cast<std::uint8_t>(Record::Finished));
        encoder.u32(errors);
        static_cast<void>(writeAll(output, encoder.view()));
    }

private:
    int output;
};

/// The status a child ends with when NVC ended it, rather than the analysis finishing.
constexpr int StoppedByNvc = 3;

/// What the child does with NVC: the work of one call, reporting through the sink it is given and
/// returning its error count.
using Job = std::function<unsigned(PipeSink&)>;

/// Runs in the child, and never returns.
[[noreturn]] void runChild(const Job& job, int records, int errors) {
    // NVC leaves a fatal error through exit(), and in a forked child exit() would run the exit
    // handlers and static destructors of the parent it is a copy of: everything Enzan registered
    // to tidy up its own process, done a second time by one that owns none of it. A handler
    // registered here is the last registered, so it runs first, and ends the child before any of
    // the parent's can.
    std::atexit([] { std::_Exit(StoppedByNvc); });

    // NVC prints what it says on its way out of a fatal error to standard error, after detaching
    // the consumer that would have translated it, so standard error is what the parent reads that
    // message from.
    ::dup2(errors, STDERR_FILENO);
    ::close(errors);

    PipeSink sink(records);
    const unsigned errorCount = job(sink);
    sink.finish(errorCount);

    // _Exit, not exit: the child is a copy of Enzan, and running Enzan's exit handlers and static
    // destructors a second time, in a process that owns none of what they release, is the one
    // thing it must not do.
    std::_Exit(errorCount == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/// Everything the child wrote to each of its two pipes, read until both are closed.
///
/// Both are drained together, since a child blocked writing one while the parent waits on the
/// other would never finish.
std::pair<std::string, std::string> drain(int records, int errors) {
    std::pair<std::string, std::string> collected;
    std::array<pollfd, 2> watched{{{.fd = records, .events = POLLIN, .revents = 0},
                                   {.fd = errors, .events = POLLIN, .revents = 0}}};
    std::array<char, 4096> chunk{};
    std::size_t open = watched.size();
    while (open > 0) {
        if (::poll(watched.data(), watched.size(), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (std::size_t index = 0; index < watched.size(); ++index) {
            pollfd& entry = watched.at(index);
            if (entry.fd < 0 || entry.revents == 0) continue;
            const ssize_t got = ::read(entry.fd, chunk.data(), chunk.size());
            if (got > 0) {
                (index == 0 ? collected.first : collected.second)
                    .append(chunk.data(), static_cast<std::size_t>(got));
            } else if (got == 0 || errno != EINTR) {
                entry.fd = -1;
                --open;
            }
        }
    }
    return collected;
}

/// The line NVC's fatal error printed, without its `** Fatal: ` heading, or a description of
/// how the child ended when it printed nothing.
std::string stopReason(std::string_view printed, int status) {
    constexpr std::string_view Heading = "** Fatal: ";
    if (const std::size_t at = printed.find(Heading); at != std::string_view::npos) {
        std::string_view line = printed.substr(at + Heading.size());
        line = line.substr(0, line.find('\n'));
        return std::string(line);
    }
    if (WIFSIGNALED(status)) {
        return std::string("NVC ended with signal ") + std::to_string(WTERMSIG(status)) + " ("
               + ::strsignal(WTERMSIG(status)) + ")";
    }
    return "NVC ended without finishing, and without saying why";
}

/// What the child's stream said.
struct Outcome {
    /// The error count, when the stream ends with the record saying the analysis finished.
    std::optional<unsigned> errors;

    /// True when NVC reported its fatal error through the consumer, as it does for a library it
    /// cannot find, rather than only printing it on its way out.
    bool stopReported = false;

    /// The translated design, when the child sent one, and the library of each definition.
    std::optional<std::string> design;
    std::vector<std::string> libraries;

    /// Where each requested resolution function is, when the child was asked for some.
    std::vector<ResolutionAnswer> answers;
};

/// Reports what the child wrote, record by record.
Outcome report(std::string_view stream, DiagnosticEngine& diagnostics) {
    Outcome outcome;
    Decoder decoder(stream);
    while (!decoder.empty()) {
        const std::optional<std::uint8_t> kind = decoder.byte();
        if (kind == static_cast<std::uint8_t>(Record::Finished)) {
            outcome.errors = decoder.u32();
            return outcome;
        }
        if (kind == static_cast<std::uint8_t>(Record::Design)) {
            outcome.design = decoder.text();
            const std::optional<std::uint32_t> count = decoder.u32();
            if (!outcome.design || !count) return outcome;
            for (std::uint32_t index = 0; index < *count; ++index) {
                std::optional<std::string> library = decoder.text();
                if (!library) return outcome;
                outcome.libraries.push_back(std::move(*library));
            }
            continue;
        }
        if (kind == static_cast<std::uint8_t>(Record::Resolutions)) {
            const std::optional<std::uint32_t> count = decoder.u32();
            if (!count) return outcome;
            const std::uint32_t total = *count;
            for (std::uint32_t index = 0; index < total; ++index) {
                std::optional<std::string> definition = decoder.text();
                std::optional<std::string> subprogram = decoder.text();
                if (!definition || !subprogram) return outcome;
                outcome.answers.push_back(ResolutionAnswer{.definition = std::move(*definition),
                                                           .subprogram = std::move(*subprogram)});
            }
            continue;
        }
        if (kind != static_cast<std::uint8_t>(Record::Diagnostic)) return outcome;

        const std::optional<std::uint32_t> code = decoder.u32();
        const std::optional<SourceLocation> location = decoder.location();
        const std::optional<std::string> message = decoder.text();
        const std::optional<std::uint32_t> noteCount = decoder.u32();
        if (!code || !location || !message || !noteCount) return outcome;
        outcome.stopReported =
            outcome.stopReported || static_cast<DiagCode>(*code) == DiagCode::VhdlAnalysisStopped;

        // Formatted already, by NVC or by the child's own engine.
        DiagnosticBuilder builder = diagnostics.reportFormatted(
            static_cast<DiagCode>(*code), location.value_or(SourceLocation()), *message);
        for (std::uint32_t index = 0; index < noteCount.value_or(0); ++index) {
            const std::optional<SourceLocation> noteLocation = decoder.location();
            std::optional<std::string> noteMessage = decoder.text();
            if (!noteLocation || !noteMessage) return outcome;
            builder.note(*noteLocation, std::move(*noteMessage));
        }
    }
    return outcome;
}

/// Runs `job` in a child process and reports what it said, returning what its stream held, or
/// nothing having reported why the child did not finish.
std::optional<Outcome> inChild(const Job& job, DiagnosticEngine& diagnostics) {
    std::array<int, 2> records{-1, -1};
    std::array<int, 2> errors{-1, -1};
    if (::pipe2(records.data(), O_CLOEXEC) != 0 || ::pipe2(errors.data(), O_CLOEXEC) != 0) {
        diagnostics.report(DiagCode::InternalError, SourceLocation(),
                           std::string("cannot create a pipe for VHDL analysis: ")
                               + std::strerror(errno));
        return std::nullopt;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(records[0]);
        ::close(errors[0]);
        runChild(job, records[1], errors[1]);
    }

    ::close(records[1]);
    ::close(errors[1]);
    if (child < 0) {
        ::close(records[0]);
        ::close(errors[0]);
        diagnostics.report(DiagCode::InternalError, SourceLocation(),
                           std::string("cannot start VHDL analysis: ") + std::strerror(errno));
        return std::nullopt;
    }

    const auto [stream, printed] = drain(records[0], errors[0]);
    ::close(records[0]);
    ::close(errors[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    Outcome outcome = report(stream, diagnostics);

    // A child that finished printed nothing NVC meant as a diagnostic, since those all came
    // through the consumer. Whatever it did print is something else — a sanitiser's report from
    // inside NVC, which carries on afterwards — and is passed on rather than lost with the pipe.
    if (outcome.errors && !printed.empty()) static_cast<void>(writeAll(STDERR_FILENO, printed));

    if (!outcome.errors) {
        // A child that a signal ended may have said why first, as an assertion of Enzan's own
        // does before it aborts; that is passed on, since the signal alone says nothing of it.
        if (WIFSIGNALED(status) && !printed.empty())
            static_cast<void>(writeAll(STDERR_FILENO, printed));
        if (!outcome.stopReported) {
            diagnostics.report(DiagCode::VhdlAnalysisStopped, SourceLocation(),
                               stopReason(printed, status));
        }
        return std::nullopt;
    }
    return outcome;
}

} // namespace

bool analyseUnits(const UnitOptions& options, const SourceManager& sources,
                  DiagnosticEngine& diagnostics) {
    const std::optional<Outcome> outcome = inChild(
        [&options, &sources](PipeSink& sink) { return runNvcAnalysis(options, sources, sink); },
        diagnostics);
    return outcome && outcome->errors == 0U;
}

namespace {

/// The definitions a child sent, decoded against the buffers this process holds, each in the
/// library the child named; nothing, having reported why, when they cannot be.
std::optional<std::vector<eir::ModuleDefinition>> definitionsFrom(const Outcome& outcome,
                                                                  const UnitOptions& units,
                                                                  const SourceManager& sources,
                                                                  DiagnosticEngine& diagnostics) {
    if (!outcome.design) {
        diagnostics.report(DiagCode::InternalError, SourceLocation(),
                           "VHDL analysis finished without sending the design");
        return std::nullopt;
    }

    // The child is a copy of this process, so the buffers it located against are these, by name.
    std::map<std::string, BufferId, std::less<>> buffers;
    for (const BufferId buffer : units.sources)
        buffers.emplace(std::string(sources.bufferName(buffer)), buffer);
    const auto resolve = [&buffers](std::string_view name) {
        const auto found = buffers.find(name);
        return found == buffers.end() ? BufferId() : found->second;
    };

    std::vector<eir::ModuleDefinition> modules;
    if (!eir::decodeModules(*outcome.design, resolve, modules)
        || modules.size() != outcome.libraries.size()) {
        diagnostics.report(DiagCode::InternalError, SourceLocation(),
                           "the design VHDL analysis sent could not be decoded");
        return std::nullopt;
    }
    for (std::size_t index = 0; index < modules.size(); ++index)
        modules[index].library = outcome.libraries[index];
    return modules;
}

} // namespace

std::unique_ptr<eir::Design> analyse(const DesignOptions& options, const SourceManager& sources,
                                     DiagnosticEngine& diagnostics) {
    const std::optional<Outcome> outcome = inChild(
        [&options, &sources](PipeSink& sink) {
            std::string encoded;
            std::vector<std::string> libraries;
            const unsigned errors = runNvcDesign(options, sources, sink, encoded, libraries);
            if (errors == 0) sink.design(encoded, libraries);
            return errors;
        },
        diagnostics);
    if (!outcome || outcome->errors != 0U) return nullptr;
    std::optional<std::vector<eir::ModuleDefinition>> modules =
        definitionsFrom(*outcome, options.units, sources, diagnostics);
    if (!modules) return nullptr;

    auto design = std::make_unique<eir::Design>();
    for (eir::ModuleDefinition& module : *modules) design->addModule(std::move(module));
    if (!eir::verify(*design, diagnostics)) return nullptr;
    return design;
}

std::optional<ResolvedFunctions> resolutions(const DesignOptions& options,
                                             const SourceManager& sources,
                                             const std::vector<ResolutionRequest>& requests,
                                             DiagnosticEngine& diagnostics) {
    const std::optional<Outcome> outcome = inChild(
        [&options, &sources, &requests](PipeSink& sink) {
            std::string encoded;
            std::vector<std::string> libraries;
            std::vector<ResolutionAnswer> answers;
            const unsigned errors =
                runNvcResolutions(options, sources, requests, sink, encoded, libraries, answers);
            if (errors == 0) {
                sink.design(encoded, libraries);
                sink.answers(answers);
            }
            return errors;
        },
        diagnostics);
    if (!outcome || outcome->errors != 0U) return std::nullopt;
    std::optional<std::vector<eir::ModuleDefinition>> modules =
        definitionsFrom(*outcome, options.units, sources, diagnostics);
    if (!modules) return std::nullopt;
    if (outcome->answers.size() != requests.size()) {
        diagnostics.report(DiagCode::InternalError, SourceLocation(),
                           "VHDL analysis answered a different number of resolution functions "
                           "than it was asked for");
        return std::nullopt;
    }
    return ResolvedFunctions{.definitions = std::move(*modules), .answers = outcome->answers};
}

std::filesystem::path standardLibraryDirectory() {
    std::error_code failure;
    const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure) return {};
    return (program.parent_path() / ".." / "lib" / "enzan" / "vhdl").lexically_normal();
}

} // namespace enzan::frontend::vhdl
