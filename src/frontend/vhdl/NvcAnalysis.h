// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "frontend/vhdl/VhdlFrontend.h"

#include <string>
#include <vector>

namespace enzan {
struct Diagnostic;
class SourceManager;
} // namespace enzan

namespace enzan::frontend::vhdl {

/// Receives the diagnostics NVC's analysis reports, already translated into Enzan's.
class DiagnosticSink {
public:
    DiagnosticSink() = default;
    DiagnosticSink(const DiagnosticSink&) = delete;
    DiagnosticSink& operator=(const DiagnosticSink&) = delete;
    DiagnosticSink(DiagnosticSink&&) = delete;
    DiagnosticSink& operator=(DiagnosticSink&&) = delete;
    virtual ~DiagnosticSink() = default;

    virtual void deliver(const Diagnostic& diagnostic) = 0;
};

/// Runs NVC's analysis over `options.sources` in this process, and returns how many errors it
/// reported.
///
/// NVC keeps its state in globals for the life of the process and ends the process on a fatal
/// error, so this is called once per process, in the child that `analyseUnits` starts for it.
///
/// Each source is handed to NVC as the text the source manager holds rather than as a file to
/// open, so that one buffer is the source of truth for both, and NVC's locations translate into
/// Enzan's exactly: a diagnostic in a source carries the location of that source's buffer. One
/// pointing into a unit some other library holds — into `ieee`, say — has no buffer here, and
/// names the file and line in a note instead.
unsigned runNvcAnalysis(const UnitOptions& options, const SourceManager& sources,
                        DiagnosticSink& sink);

/// Analyses as `runNvcAnalysis` does and translates the design under `options.top` into EIR,
/// returning the number of errors either reported.
///
/// `encoded` receives the translated definitions as `eir::encodeModules` writes them, when there
/// were no errors, `libraries` the library of each in order, which that encoding leaves out, and
/// `exchange` what the design exchanges with SystemVerilog (`translateDesign`).
unsigned runNvcDesign(const DesignOptions& options, const SourceManager& sources,
                      DiagnosticSink& sink, std::string& encoded,
                      std::vector<std::string>& libraries, Exchange& exchange);

/// Analyses as `runNvcAnalysis` does and translates each of `requests`, filling `encoded` and
/// `libraries` as `runNvcDesign` does and `answers` with where each request's function is.
unsigned runNvcResolutions(const DesignOptions& options, const SourceManager& sources,
                           const std::vector<ResolutionRequest>& requests, DiagnosticSink& sink,
                           std::string& encoded, std::vector<std::string>& libraries,
                           std::vector<ResolutionAnswer>& answers);

} // namespace enzan::frontend::vhdl
