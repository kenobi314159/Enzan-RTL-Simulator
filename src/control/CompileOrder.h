// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::control {

/// What a declared name denotes.
enum class UnitKind : std::uint8_t {
    Module,
    Package,
    Interface,
    Program,
    Primitive,
    Class,
};

/// Returns the keyword a kind is declared with, for diagnostics.
[[nodiscard]] std::string_view toString(UnitKind kind) noexcept;

/// One design unit a file declares.
struct Declaration {
    UnitKind kind = UnitKind::Module;
    std::string name;
    SourceLocation location;
};

/// One place a file names a package: `import pkg::*`, or `pkg::THING`.
struct PackageReference {
    std::string name;
    SourceLocation location;
};

/// What the pre-scan found in one file.
struct SourceScan {
    std::vector<Declaration> declarations;
    std::vector<PackageReference> packageReferences;
};

/// Extracts declared and referenced names from SystemVerilog source without parsing it.
///
/// Section 8.3 requires this to be **much cheaper than full parsing**, because it runs over
/// every file before any analysis happens. It is one linear pass over the bytes: comments and
/// string literals are skipped, identifiers are recognised, and a small stack tracks whether a
/// keyword is at file scope or inside something.
///
/// It is deliberately approximate, and the direction of the approximation is chosen. A
/// declaration it invents costs nothing, because a declaration only ever *satisfies* a
/// dependency. A package reference it invents could invent an ordering constraint, so a
/// reference only becomes an edge when the name it uses is a package some other scanned file
/// declares (`inferCompileOrder`): `cls::CONST` naming a class, or `chandle::x` naming
/// anything at all, resolves to no package and orders nothing.
///
/// Conditional compilation is not evaluated: text inside a `` `ifdef `` that would be false is
/// scanned like any other. Analysing a package that nothing ends up importing is harmless; not
/// analysing one that something does is not.
[[nodiscard]] SourceScan scanSystemVerilog(BufferId buffer, std::string_view text);

/// Orders `files` so that every file is analysed after whatever it depends on.
///
/// Users MUST NOT have to state compile order (section 8.3), and in Enzan's analysis model
/// they genuinely cannot be asked to: analysis is per design unit (section 2.1), so a module
/// that imports a package needs that package in the library already.
///
/// **Instantiating a module creates no ordering constraint.** Elaboration resolves an instance
/// against the library by name (section 2.1), long after both files are analysed, so two
/// modules that instantiate each other are the "legal mutual reference" section 8.3 says MUST
/// NOT be reported as a cycle. Only a package reference orders anything, and a genuine cycle
/// between packages is reported with the cycle spelled out.
///
/// Files that do not depend on each other keep the order they were given, because analysis
/// order is observable — in diagnostics, and in which of two duplicate definitions is reported
/// as the redefinition — and section 3.8 makes observable ordering reproducible.
///
/// Returns false having reported through `diagnostics`. Files that cannot be read are reported
/// and the ordering fails, because an unreadable file cannot be shown to be free of
/// dependencies.
bool inferCompileOrder(std::span<const std::filesystem::path> files, SourceManager& sources,
                       DiagnosticEngine& diagnostics, std::vector<std::filesystem::path>& ordered);

} // namespace enzan::control
