// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/UnitRequest.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::eir {
class Design;
struct ModuleDefinition;
} // namespace enzan::eir

/// The VHDL frontend.
///
/// A frontend's only job is to turn source text into EIR (DESIGN_GUIDE.md section 5). This one is
/// built on NVC's analysis, and nothing NVC-specific may leak past this directory: keeping
/// frontend-specific code confined is a standing invariant.
namespace enzan::frontend::vhdl {

/// The revision of IEEE 1076 a design is analysed against.
///
/// Part of what an analysis produces, so part of a library entry's key, and it chooses which
/// of the standard libraries' revisions a unit is analysed against.
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
    /// The sources, read by the caller.
    ///
    /// The order matters in VHDL as it does not in SystemVerilog: a unit is analysed against the
    /// units already analysed (IEEE 1076 section 13.1), so a package comes before what uses it.
    /// They are analysed in an order that puts each after the sources declaring the units it
    /// names, and otherwise in this one (`dependencyOrder`).
    std::vector<BufferId> sources;

    /// Where the analysed units go. With no directory, they are held in memory for the run and
    /// written nowhere.
    UnitLibrary work;

    /// Libraries a design refers to by name, each where its units are: the VHDL of the Enzan
    /// libraries a design is analysed against (`--in-lib`). Found before anything in
    /// `searchDirectories`.
    std::vector<UnitLibrary> libraries;

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
/// names them, once per set of generic values.
///
/// **NVC runs in a child process**, one per call. NVC keeps its state in globals that last for
/// the life of a process, and it ends the process on a fatal error — a library a design names that
/// does not exist is one — so running it in Enzan's own process would make a second analysis
/// see the first one's libraries, and a user's mistake end the simulator. The child reports each
/// of NVC's diagnostics to `diagnostics` as if it had run here, in the order NVC gave them; a
/// fatal error stops the child and is reported as ENZ2004, carrying NVC's own message, unless it
/// is a library the design uses and the run was not given (ENZ2009, ENZ2010).
///
/// Returns true when every source analysed without error and the library was written.
bool analyseUnits(const UnitOptions& options, const SourceManager& sources,
                  DiagnosticEngine& diagnostics);

/// What to simulate: the sources to analyse, and the entity to translate the design from.
struct DesignOptions {
    /// Analysed first, into `units.work`, as `analyseUnits` would.
    UnitOptions units;

    /// The top entity, in any case. Empty means the one entity the sources declare that none of
    /// their architectures instantiates; with several, none is translated and they are reported.
    std::string top;

    /// The top's generics `-G` sets, each `NAME=VALUE`: the name in any case, and the value as
    /// NVC's and GHDL's `-g` read one of the generic's type. They apply where the design is
    /// translated rather than where it is analysed, so they are no part of a library's key.
    std::vector<std::string> generics;

    /// The unit EIR's delays count, as a power of ten of a second.
    std::int8_t timeUnitExponent = -15;

    /// Whether the IEEE packages' own warnings, numeric_std's of metavalues and truncation, are
    /// reported, as GHDL's `--ieee-asserts`: they are guarded by a constant NVC's runtime would
    /// give, which the adapter folds to this.
    bool ieeeWarnings = true;

    /// The SystemVerilog modules of a design of both languages, by their names as declared: a
    /// component with no entity to be bound to is bound to the module of its name, in any
    /// case.
    std::vector<std::string> modules;

    /// The entities SystemVerilog instantiates, each translated with the values it asks for, the
    /// name of the definition each becomes answered in `Exchange::answers`.
    std::vector<eir::UnitRequest> requests;

    /// The names SystemVerilog instantiates and declares nowhere, of which each an entity has is
    /// described, in `Exchange::interfaces`, and none is a top level.
    std::vector<std::string> described;

    /// Whether nothing but `requests` is translated, as when the top level is SystemVerilog's.
    bool onlyRequested = false;
};

/// What a translation of the VHDL of a design of both languages exchanges with the SystemVerilog
/// side.
struct Exchange {
    /// The SystemVerilog modules the design instantiates, which its instances name placeholders of.
    std::vector<eir::UnitRequest> asked;

    /// The definition each of `DesignOptions::requests` became, in order.
    std::vector<std::string> answers;

    /// The interface of each of `DesignOptions::described` that an entity has, named as asked, its
    /// generics and ports named as declared.
    std::vector<eir::UnitInterface> interfaces;
};

/// Analyses `options.units` and translates the design under the top into EIR.
///
/// In the child process `analyseUnits` describes, which hands the parent the translated design as
/// EIR's own encoding: NVC's trees live in the child and end with it, and nothing of NVC reaches
/// the parent's memory.
///
/// An instance of one of `options.modules` names a placeholder as its definition, and `exchange`,
/// when given, receives what each placeholder asks for, for the SystemVerilog frontend to translate
/// and the session to link, with what the design answers SystemVerilog.
///
/// Returns nothing having reported why, when a source does not analyse or the design uses
/// something the adapter does not translate yet. `analysed`, when given, is set to whether the
/// sources analysed, which leaves their units in the work library even where the translation
/// after it failed.
std::unique_ptr<eir::Design> analyse(const DesignOptions& options, const SourceManager& sources,
                                     DiagnosticEngine& diagnostics, Exchange* exchange = nullptr,
                                     bool* analysed = nullptr);

/// A resolution function asked for by how many drivers it resolves: `function` names it as
/// `eir::Resolution` does.
struct ResolutionRequest {
    std::string function;
    std::uint32_t drivers = 0;

    friend bool operator==(const ResolutionRequest&, const ResolutionRequest&) = default;
};

/// Where a requested resolution function is: a subprogram of a definition.
struct ResolutionAnswer {
    std::string definition;
    std::string subprogram;
};

/// What `resolutions` translated: the definitions holding the functions, and where each request's
/// is, in the order they were asked for.
struct ResolvedFunctions {
    std::vector<eir::ModuleDefinition> definitions;
    std::vector<ResolutionAnswer> answers;
};

/// Analyses `options.units` again and translates each of `requests`, its function specialised to
/// an array of as many values as it has drivers, indexed from 0, in a child as `analyse`
/// translates a design. Elaboration asks for these once it has counted a resolved signal's
/// drivers, which the first translation could not know.
///
/// Returns nothing having reported why, when a source does not analyse again or a function is
/// not one the adapter can translate.
std::optional<ResolvedFunctions> resolutions(const DesignOptions& options,
                                             const SourceManager& sources,
                                             const std::vector<ResolutionRequest>& requests,
                                             DiagnosticEngine& diagnostics);

/// The primary units `source` declares, `entity`, `package`, `configuration` and `context`, by
/// their names in lower case: what a library records it holding, without analysing anything.
[[nodiscard]] std::vector<std::string> declaredUnits(BufferId source, const SourceManager& sources);

/// Where the standard libraries built with Enzan are, found from the directory of the running
/// program, which is where a build puts them and an installation keeps them.
std::filesystem::path standardLibraryDirectory();

} // namespace enzan::frontend::vhdl
