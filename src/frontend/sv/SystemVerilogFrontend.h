// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Design.h"

#include <cstdint>
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

    /// Overrides of the top level's parameters, each `NAME=VALUE` (`enzan -G`).
    std::vector<std::string> parameterOverrides;

    /// The time unit, as a power of ten of a second, that a design element without a
    /// `` `timescale `` or `timeunit` of its own is taken to be in, as its unit and its
    /// precision, and the run's unit unless `refineTimeUnit` lets a finer one replace it.
    ///
    /// IEEE 1800 makes a design where some elements have a time scale and others do not an
    /// error, and slang enforces it; other simulators warn, and real designs — a testbench with a
    /// `` `timescale `` and the RTL without — rely on that.
    std::int8_t timeUnitExponent = -12;

    /// Whether the run's unit is the finest precision an element of the design declares, where
    /// that is finer than `timeUnitExponent` (DESIGN_GUIDE.md section 3.1).
    ///
    /// Off when `--timescale` or a library fixes the unit. A delay that is no whole number of a
    /// fixed unit is rounded to it and reported.
    bool refineTimeUnit = false;

    /// Receives the run's unit, which every delay of the design counts, when given.
    std::int8_t* runTimeUnit = nullptr;

    /// Receives every file the analysis read, when given.
    ///
    /// The sources handed in *and* the files they included, which is the part a caller cannot
    /// work out for itself: an `` `include `` is resolved by the preprocessor against the
    /// include path, and a build that does not know which header a source actually opened
    /// cannot rebuild when that header changes (DESIGN_GUIDE.md section 8.3).
    std::vector<std::filesystem::path>* filesRead = nullptr;
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
