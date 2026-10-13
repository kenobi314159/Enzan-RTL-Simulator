// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/NvcAnalysis.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Serialization.h"
#include "frontend/vhdl/NvcApi.h"
#include "frontend/vhdl/Translator.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// What NVC's diagnostic callback needs, handed to it through its context pointer.
struct Translation {
    const SourceManager& sources;
    DiagnosticSink& sink;

    /// The buffer of each source NVC was given, by the file reference NVC assigned it.
    std::vector<std::pair<file_ref_t, BufferId>> files;
};

/// Removes NVC's terminal markup from a message.
///
/// NVC writes its messages for a terminal and marks styles inline: `$bold$` and the colour names
/// open one and `$$` closes it (src/printf.c). Enzan's consumers style a diagnostic themselves, so
/// the markup is dropped and the text kept. A `$` that opens nothing NVC knows is left alone, since
/// it is then part of the message — a VHDL bit string literal never contains one, but a file name
/// may.
std::string stripMarkup(std::string_view text) {
    static constexpr std::string_view Styles[] = {
        "", "bold", "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
    };

    std::string plain;
    plain.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == '$') {
            const std::size_t close = text.find('$', at + 1);
            if (close != std::string_view::npos) {
                const std::string_view name = text.substr(at + 1, close - at - 1);
                bool isStyle = false;
                for (const std::string_view style : Styles) isStyle = isStyle || name == style;
                if (isStyle) {
                    at = close + 1;
                    continue;
                }
            }
        }
        plain += text[at];
        ++at;
    }
    return plain;
}

/// The buffer of the source NVC knows as `file`, or an invalid one when it is not one of the
/// sources handed in.
BufferId bufferOf(const Translation& translation, file_ref_t file) {
    for (const auto& [reference, buffer] : translation.files) {
        if (reference == file) return buffer;
    }
    return {};
}

/// Translates an NVC location into a position in its source's buffer.
///
/// NVC counts lines from one and columns from zero, which is the convention of a byte offset into
/// the line, so the column is added to the start of the line as it stands.
SourceLocation locate(const Translation& translation, const loc_t* location) {
    if (location == nullptr || loc_invalid_p(location)) return {};

    const BufferId buffer = bufferOf(translation, location->file_ref);
    if (!buffer.valid()) return {};

    const std::uint32_t line = location->first_line;
    if (line == 0 || line > translation.sources.lineCount(buffer)) return {};

    const SourceLocation start = translation.sources.lineStart(buffer, line);
    const std::uint32_t lineLength =
        static_cast<std::uint32_t>(translation.sources.lineText(start).size());
    return start.advanced(location->first_column <= lineLength ? location->first_column : 0U);
}

/// The diagnostic code an NVC level becomes, or nothing for a level Enzan does not report.
bool codeFor(diag_level_t level, DiagCode& code) {
    switch (level) {
    case DIAG_DEBUG: return false;
    case DIAG_NOTE: code = DiagCode::VhdlNote; return true;
    case DIAG_WARN: code = DiagCode::VhdlWarning; return true;
    case DIAG_ERROR:
    case DIAG_FAILURE: code = DiagCode::VhdlError; return true;
    case DIAG_FATAL: code = DiagCode::VhdlAnalysisStopped; return true;
    }
    return false;
}

/// NVC's diagnostic consumer: translates one of NVC's diagnostics and hands it to the sink.
///
/// Registering a consumer stops NVC printing the diagnostic itself, and it still counts it, so
/// error_count() goes on meaning what NVC's analysis loop relies on it meaning.
void translate(diag_t* nvcDiagnostic, void* context) {
    const Translation& translation = *static_cast<const Translation*>(context);

    DiagCode code = DiagCode::VhdlError;
    if (!codeFor(diag_level(nvcDiagnostic, nullptr), code)) return;

    Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = defaultSeverity(code);
    diagnostic.message = stripMarkup(diag_get_text(nvcDiagnostic));

    const loc_t* const location = diag_get_loc(nvcDiagnostic);
    diagnostic.location = locate(translation, location);
    if (!diagnostic.location.valid() && location != nullptr && !loc_invalid_p(location)) {
        // A position in a file that is not one of the sources: a unit of another library.
        diagnostic.notes.push_back({.location = {},
                                    .message = std::string("at ") + loc_file_str(location) + ":"
                                               + std::to_string(location->first_line)});
    }

    const int hints = diag_hints(nvcDiagnostic);
    for (int index = 0; index < hints; ++index) {
        diagnostic.notes.push_back(
            {.location = {}, .message = stripMarkup(diag_get_hint(nvcDiagnostic, index))});
    }

    translation.sink.deliver(diagnostic);
}

vhdl_standard_t nvcStandard(Standard standard) {
    switch (standard) {
    case Standard::Vhdl1993: return STD_93;
    case Standard::Vhdl2000: return STD_00;
    case Standard::Vhdl2002: return STD_02;
    case Standard::Vhdl2008: return STD_08;
    case Standard::Vhdl2019: return STD_19;
    }
    return STD_08;
}

/// Forwards what the adapter reports to the sink NVC's own diagnostics go to, so that the parent
/// receives both as one stream in the order they were made.
class SinkConsumer final : public DiagnosticConsumer {
public:
    explicit SinkConsumer(DiagnosticSink& destination) noexcept : sink(destination) {}

    void consume(const Diagnostic& diagnostic) override { sink.deliver(diagnostic); }

private:
    DiagnosticSink& sink;
};

/// NVC, set up for one run, and what the run analysed.
class Run {
public:
    Run(const UnitOptions& unitOptions, const SourceManager& sourceManager, DiagnosticSink& sink)
        : options(unitOptions),
          sources(sourceManager),
          translation{.sources = sourceManager, .sink = sink, .files = {}} {
        // What NVC's own driver does before anything else (src/nvc.c), less what concerns a
        // terminal or the simulation it would go on to run.
        term_init();
        thread_init();
        set_default_options();
        intern_strings();

        set_standard(nvcStandard(options.standard));
        if (options.bootstrap) opt_set_int(OPT_BOOTSTRAP, 1);
        for (const std::filesystem::path& directory : options.searchDirectories)
            lib_add_search_path(directory.c_str());

        diag_set_consumer(translate, &translation);

        // NAME:DIRECTORY names the library and fixes where it lives, rather than searching for
        // it; with no directory the library is NVC's in-memory one, which nothing writes out.
        if (options.work.directory.empty()) {
            work = lib_tmp(options.work.name.c_str());
        } else {
            const std::string spec = options.work.name + ":" + options.work.directory.string();
            work = lib_new(spec.c_str());
        }
        lib_set_work(work);
    }

    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;
    Run(Run&&) = delete;
    Run& operator=(Run&&) = delete;

    ~Run() { diag_set_consumer(nullptr, nullptr); }

    /// Exactly the loop of NVC's `nvc -a` (src/nvc.c), with the constant evaluator replaced
    /// (third_party/nvc-analysis/enzan_nvc_seam.c), so simplification is given no JIT. Returns
    /// the error count.
    unsigned analyse() {
        for (const BufferId buffer : options.sources) {
            const std::string name(sources.bufferName(buffer));
            const std::string_view text = sources.bufferText(buffer);
            // NVC keeps the pointer to quote lines from, which it stops at a newline; the source
            // manager's text is terminated besides.
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
            const file_ref_t reference = loc_file_ref(name.c_str(), text.data());
            translation.files.emplace_back(reference, buffer);

            input_from_buffer(text.data(), text.size(), reference, SOURCE_VHDL);
            for (;;) {
                const unsigned before = error_count();
                tree_t unit = parse();
                if (unit == nullptr) break;

                if (error_count() == before) {
                    lib_put(work, unit);
                    simplify_local(unit, nullptr, nullptr, nullptr);
                    bounds_check(unit);
                    units.push_back(unit);
                } else {
                    lib_put_error(work, unit);
                }
            }
        }

        if (error_count() == 0 && !options.work.directory.empty()) lib_save(work);
        return error_count();
    }

    [[nodiscard]] SourceLocation locate(const loc_t* location) const {
        return vhdl::locate(translation, location);
    }

    [[nodiscard]] lib_t workLibrary() const noexcept { return work; }

    [[nodiscard]] const std::vector<tree_t>& analysed() const noexcept { return units; }

private:
    const UnitOptions& options;
    const SourceManager& sources;
    Translation translation;
    lib_t work = nullptr;
    std::vector<tree_t> units;
};

} // namespace

unsigned runNvcAnalysis(const UnitOptions& options, const SourceManager& sources,
                        DiagnosticSink& sink) {
    Run run(options, sources, sink);
    return run.analyse();
}

unsigned runNvcDesign(const DesignOptions& options, const SourceManager& sources,
                      DiagnosticSink& sink, std::string& encoded,
                      std::vector<std::string>& libraries) {
    Run run(options.units, sources, sink);
    const unsigned analysisErrors = run.analyse();
    if (analysisErrors != 0) return analysisErrors;

    DiagnosticEngine engine(sources);
    SinkConsumer consumer(sink);
    engine.addConsumer(consumer);

    const DesignRequest request{.top = options.top,
                                .timeUnitExponent = options.timeUnitExponent,
                                .work = run.workLibrary(),
                                .analysed = run.analysed()};
    const std::vector<eir::ModuleDefinition> modules = translateDesign(
        request, [&run](const loc_t* location) { return run.locate(location); }, engine);

    const unsigned errors = engine.errorCount() + error_count();
    if (errors == 0) {
        encoded = eir::encodeModules(modules, sources);
        for (const eir::ModuleDefinition& definition : modules)
            libraries.push_back(definition.library);
    }
    engine.removeConsumer(consumer);
    return errors;
}

unsigned runNvcResolutions(const DesignOptions& options, const SourceManager& sources,
                           const std::vector<ResolutionRequest>& requests, DiagnosticSink& sink,
                           std::string& encoded, std::vector<std::string>& libraries,
                           std::vector<ResolutionAnswer>& answers) {
    Run run(options.units, sources, sink);
    const unsigned analysisErrors = run.analyse();
    if (analysisErrors != 0) return analysisErrors;

    DiagnosticEngine engine(sources);
    SinkConsumer consumer(sink);
    engine.addConsumer(consumer);

    const DesignRequest request{.top = options.top,
                                .timeUnitExponent = options.timeUnitExponent,
                                .work = run.workLibrary(),
                                .analysed = run.analysed()};
    const std::vector<eir::ModuleDefinition> modules = translateResolutions(
        request, requests, answers, [&run](const loc_t* location) { return run.locate(location); },
        engine);

    const unsigned errors = engine.errorCount() + error_count();
    if (errors == 0) {
        encoded = eir::encodeModules(modules, sources);
        for (const eir::ModuleDefinition& definition : modules)
            libraries.push_back(definition.library);
    }
    engine.removeConsumer(consumer);
    return errors;
}

} // namespace enzan::frontend::vhdl
