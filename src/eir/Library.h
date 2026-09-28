// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::eir {

class Design;
struct ModuleDefinition;

/// One source file as the library sees it: its path, its text, and where its text lives.
///
/// The text is carried rather than re-read because the caller has already read it — the key
/// is a hash of it — and reading a file twice to compute the same number would be the one
/// cost a cache exists to avoid.
struct LibrarySource {
    std::filesystem::path path;

    /// The buffer holding `path`'s text in the caller's source manager.
    BufferId buffer;
};

/// The analysis library: a directory of serialised design units (DESIGN_GUIDE.md section 4.4).
///
/// `work/` by default, mirroring the convention users know from Questa. An entry is one source
/// file's analysed units, named by a hash of that file's text together with everything else
/// that changes what analysis produces — the macro definitions, the include path, the format
/// version and Enzan's own version.
///
/// **A hit is all or nothing, and that is a property of the frontend rather than of the
/// store.** SystemVerilog analysis today hands every source to one slang compilation, because
/// a module cannot be type-checked without the packages it imports; so either every file's
/// entry is present and current, in which case nothing is analysed at all, or one is missing
/// and everything is analysed and every entry rewritten. The entries are nonetheless kept per
/// file, because that is the shape section 4.4 describes and the shape a per-unit frontend
/// will want; only the hit rule would change.
///
/// **The library is opt-in.** Nothing uses it until a session opens one, exactly as Questa
/// requires a library to be created before it is used. That is not only convention: a cache
/// that silently served EIR
/// produced by a different translator would give a different simulation result, and section
/// 1.2 ranks that worst of all. The version is in the key, and the format version must be
/// bumped whenever the translation changes (see Serialization.h), but an opt-in cache means a
/// user who suspects one has an obvious thing to stop doing.
class Library {
public:
    /// Opens, creating the directory if it does not exist.
    ///
    /// Returns false having reported through `diagnostics` when the path exists and is not a
    /// directory, or cannot be created.
    [[nodiscard]] bool open(const std::filesystem::path& directory, DiagnosticEngine& diagnostics);

    [[nodiscard]] bool isOpen() const noexcept { return opened; }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return directory; }

    /// Sets the analysis options that take part in every entry's key.
    ///
    /// Anything that changes what analysis produces belongs here. A define that is not in the
    /// key is a define whose change is invisible to the cache, which is a wrong simulation
    /// rather than a slow one.
    void setOptions(std::span<const std::string> defines,
                    std::span<const std::filesystem::path> includeDirectories);

    /// Loads every source's units into `design`, or reports why it could not.
    ///
    /// Returns true only when *every* source had a current entry and all of them decoded; in
    /// that case `design` holds the whole analysis and the frontend need not run. Returns
    /// false having left `design` untouched otherwise, which is the ordinary case on a first
    /// run and after any edit, and is not an error.
    ///
    /// `resolveBuffer` maps a stored buffer name onto a buffer in `sources`; the caller
    /// supplies it because only the caller knows which files it is willing to read.
    [[nodiscard]] bool load(std::span<const LibrarySource> sources,
                            const SourceManager& sourceManager, Design& design);

    /// Stores each source's units, replacing whatever was there.
    ///
    /// Units are attributed to a source by the buffer their definition's location names, so a
    /// module defined in a file nobody listed — or with no location at all — is not stored.
    /// That costs a cache hit and never costs correctness, because a missing entry is a miss
    /// and a miss re-analyses everything.
    ///
    /// Returns false having reported when a file could not be written. A failed store is not
    /// a failed analysis: the caller has the design either way.
    bool store(std::span<const LibrarySource> sources, const SourceManager& sourceManager,
               const Design& design, DiagnosticEngine& diagnostics);

    /// The number of entries `load` found, for the transcript line that says so.
    [[nodiscard]] std::size_t entriesLoaded() const noexcept { return loadedCount; }

private:
    /// The file an entry for `source` lives in.
    [[nodiscard]] std::filesystem::path entryPath(const LibrarySource& source,
                                                  const SourceManager& sourceManager) const;

    std::filesystem::path directory;

    /// The part of every key that does not depend on the file: the options, the format
    /// version and Enzan's version. Computed once when the options are set.
    std::string optionsDigest;

    std::size_t loadedCount = 0;
    bool opened = false;
};

} // namespace enzan::eir
