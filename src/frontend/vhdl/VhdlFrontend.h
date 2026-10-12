// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::eir {
class Design;
}

/// The VHDL frontend.
///
/// A frontend's only job is to turn source text into EIR (DESIGN_GUIDE.md section 5). This one is
/// built on NVC's analysis (section 5.2), and nothing NVC-specific may leak past this directory:
/// keeping frontend-specific code confined is a standing invariant (section 17.4).
namespace enzan::frontend::vhdl {

/// The revision of IEEE 1076 a design is analysed against.
///
/// Part of what an analysis produces, so part of a library entry's key (section 4.4), and it
/// chooses which of the standard libraries' revisions a unit is analysed against.
enum class Standard : std::uint8_t { Vhdl1993, Vhdl2000, Vhdl2002, Vhdl2008, Vhdl2019 };

/// A library that NVC's analysis writes units into.
struct UnitLibrary {
    /// The logical name a design refers to it by: `work`, `ieee`, `uart`.
    std::string name;

    /// The directory holding its units, in NVC's format.
    std::filesystem::path directory;
};

/// What to analyse, and into which library.
struct UnitOptions {
    /// The sources, read by the caller, in the order they are to be analysed.
    ///
    /// The order matters in VHDL as it does not in SystemVerilog: a unit is analysed against the
    /// units already analysed (IEEE 1076 section 13.1), so a package comes before what uses it.
    std::vector<BufferId> sources;

    /// Where the analysed units go. With no directory, they are held in memory for the run and
    /// written nowhere.
    UnitLibrary work;

    /// Directories holding the other libraries a design refers to, searched in order.
    ///
    /// Each library is a subdirectory named for it, as NVC lays them out, with the standard's
    /// suffix where there is one: `std.08`, `ieee.08`.
    std::vector<std::filesystem::path> searchDirectories;

    Standard standard = Standard::Vhdl2008;

    /// Analyses `std.standard` itself, which NVC does only in its bootstrap mode, since every
    /// other unit is analysed against it. For building the standard libraries and nothing else.
    bool bootstrap = false;
};

/// Analyses VHDL sources with NVC's analysis, into NVC's units in `options.work`.
///
/// This is analysis as VHDL defines it: each unit parsed, checked and stored, against the units
/// stored before it. It produces no EIR; the adapter translates units into EIR when a design
/// names them, once per set of generic values (section 5.2).
///
/// **NVC runs in a child process**, one per call. NVC keeps its state in globals that last for
/// the life of a process, and it ends the process on a fatal error — a library a design names that
/// does not exist is one — so running it in Enzan's own process would make a second analysis
/// see the first one's libraries, and a user's mistake end the simulator. The child reports each
/// of NVC's diagnostics to `diagnostics` as if it had run here, in the order NVC gave them; a
/// fatal error stops the child and is reported as ENZ2004, carrying NVC's own message.
///
/// Returns true when every source analysed without error and the library was written.
bool analyseUnits(const UnitOptions& options, const SourceManager& sources,
                  DiagnosticEngine& diagnostics);

/// What to simulate: the sources to analyse, and the entity to translate the design from.
struct DesignOptions {
    /// Analysed first, into `units.work`, as `analyseUnits` would.
    UnitOptions units;

    /// The top entity, in any case. Empty means every entity the sources declare that none of their
    /// architectures instantiates.
    std::string top;

    /// The unit EIR's delays count, as a power of ten of a second.
    std::int8_t timeUnitExponent = -15;
};

/// Analyses `options.units` and translates the design under the top into EIR (section 5.2).
///
/// In the child process `analyseUnits` describes, which hands the parent the translated design as
/// EIR's own encoding: NVC's trees live in the child and end with it, and nothing of NVC reaches
/// the parent's memory.
///
/// Returns nothing having reported why, when a source does not analyse or the design uses
/// something the adapter does not translate yet.
std::unique_ptr<eir::Design> analyse(const DesignOptions& options, const SourceManager& sources,
                                     DiagnosticEngine& diagnostics);

/// Where the standard libraries built with Enzan are: `lib/enzan/vhdl` beside the directory of the
/// running program, which is where a build puts them and an installation keeps them (section 4.4).
std::filesystem::path standardLibraryDirectory();

} // namespace enzan::frontend::vhdl
