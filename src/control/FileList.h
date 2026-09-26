// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::control {

/// Everything a file list can name.
///
/// The fields mirror the options of DESIGN_GUIDE.md section 8.2 rather than the frontend's
/// option structure, because the two differ in one way that matters: a file list distinguishes
/// sources that MUST be analysed from libraries that are analysed only if something refers to
/// them, and flattening that distinction here would analyse a whole vendor library because one
/// module of it was wanted.
struct FileList {
    /// Sources to analyse, in the order the lists named them.
    ///
    /// Duplicates are kept. A design that names a file twice is doing something the user can
    /// see and fix, and silently dropping the second mention would differ from every other tool
    /// that reads these files.
    std::vector<std::filesystem::path> files;

    /// `-v`: files holding modules to analyse only when something instantiates one.
    ///
    /// Collected but not yet acted on: nothing searches for an undefined module, so reading a
    /// list that relies on one warns where the option is. Recorded rather than discarded because
    /// the search is what these fields are for and the reader is already right.
    std::vector<std::filesystem::path> libraryFiles;

    /// `-y`: directories searched for `<module><ext>` when something instantiates a module
    /// nothing analysed defines. Collected but not yet acted on, as `libraryFiles` is.
    std::vector<std::filesystem::path> libraryDirectories;

    /// `+libext+`: the extensions tried in a `-y` directory, in order. Empty means `.v`.
    std::vector<std::string> libraryExtensions;

    /// `+incdir+`: the `` `include `` search path.
    std::vector<std::filesystem::path> includeDirectories;

    /// `+define+`: macro definitions, each `NAME` or `NAME=value`.
    std::vector<std::string> defines;
};

/// Reads `path` as a `.f` file list, appending what it names to `result`.
///
/// `.f` is the de-facto industry standard and nearly every open-source design on GitHub ships
/// one (section 8.2). Requiring a user to translate one into Tcl before Enzan will look at
/// their design is friction with nothing on the other side of it.
///
/// Supported: `-f` and `-F` for nesting, `-v`, `-y`, `+incdir+`, `+define+`, `+libext+`, `-sv`,
/// `//` and `/* */` comments, double-quoted arguments, and `$VAR` and `${VAR}` substitution
/// from the environment.
///
/// **Relative paths follow the convention the files were written against**, which is not the
/// obvious one: a path inside a list read with `-f` resolves against the current working
/// directory, and one inside a list read with `-F` resolves against that list's own directory.
/// Enzan copies this rather than picking the sane rule, because the whole value of reading
/// these files is that existing ones work unchanged.
///
/// **An option Enzan does not recognise is an error, not a warning.** File lists carry
/// tool-specific switches that change what is compiled — a timescale, a compilation-unit mode,
/// a language version — and quietly ignoring one produces a design that analyses and simulates
/// differently from what the user asked for. That is the failure mode section 1.2 weighs above
/// all others, so an unknown option stops with its position named.
///
/// Returns false having reported through `diagnostics`. `result` may hold what was read up to
/// the failure, so a caller that failed must discard it rather than use it.
bool readFileList(const std::filesystem::path& path, SourceManager& sources,
                  DiagnosticEngine& diagnostics, FileList& result);

} // namespace enzan::control
