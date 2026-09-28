// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Design.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

/// The SystemVerilog frontend.
///
/// A frontend's only job is to turn source text into EIR (DESIGN_GUIDE.md section 5). It
/// MUST NOT flatten hierarchy, make scheduling decisions or allocate runtime state, and
/// nothing slang-specific may leak past this directory — keeping frontend-specific code
/// confined is a standing invariant (section 17.4), and a slang type in an EIR header
/// would make every downstream component depend on slang.
namespace enzan::frontend::sv {

/// What to analyse.
struct Options {
    std::vector<std::filesystem::path> files;

    /// Buffers already holding each file's text, parallel to `files`, or empty.
    ///
    /// The caller has already read every source, so handing the buffers over saves the
    /// frontend reading them again. That is not only about the I/O: the source manager does not
    /// deduplicate, so a second read leaves *two* buffers per file, and a SourceLocation's
    /// buffer then depends on which pass produced it.
    /// Two things downstream compare locations by buffer — the analysis library, which
    /// attributes units to files, and a source-line breakpoint — and both were silently
    /// matching nothing.
    ///
    /// An entry may be invalid, which makes the frontend read that file itself.
    std::vector<BufferId> buffers;

    /// Macro definitions, each either `NAME` or `NAME=VALUE`.
    std::vector<std::string> defines;

    std::vector<std::filesystem::path> includeDirectories;

    /// Top-level modules to elaborate. Empty lets slang choose, which it does by taking
    /// every module nothing else instantiates.
    ///
    /// This is slang's notion of a top level, used to decide which instance bodies exist
    /// at all. Enzan's own choice of which one to simulate happens at elaboration.
    std::vector<std::string> topModules;
};

/// Analyses SystemVerilog sources into EIR.
///
/// Source text is read through Enzan's own source manager so that a diagnostic from slang
/// and a diagnostic from elaboration quote the same buffer with the same snippet. slang is
/// then handed that text rather than the file, which keeps a single copy in memory and
/// makes the two managers agree on offsets.
///
/// Returns nullptr having reported diagnostics when the sources do not analyse, or when
/// they use a construct the frontend does not support yet. A partially translated design
/// is never returned: it would simulate and give the wrong answer, which section 1.2 ranks
/// as the worst possible outcome.
std::unique_ptr<eir::Design> analyse(const Options& options, SourceManager& sources,
                                     DiagnosticEngine& diagnostics);

} // namespace enzan::frontend::sv
