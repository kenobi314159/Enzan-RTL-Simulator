// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Library.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "common/Version.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Serialization.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace enzan::eir {

namespace {

/// FNV-1a over 128 bits, rendered as the 32 hex characters an entry is named by.
///
/// 128 bits rather than 64 because a collision here is not a slow build but a wrong one: two
/// different sources hashing alike would load one design and simulate another. At 128 bits the
/// probability is not worth a sentence; at 64 it would be worth thinking about. `__uint128_t`
/// is a compiler extension and Enzan is GCC-or-Clang on Linux only (section 1.3), so this
/// costs nothing that is not already spent.
class Digest {
public:
    void add(std::string_view bytes) {
        for (const char byte : bytes) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= Prime;
        }
        // A separator, so that ("ab", "c") and ("a", "bc") do not hash alike. Two fields that
        // concatenate to the same bytes are exactly how a cache key stops distinguishing a
        // define from an include path.
        hash ^= 0xFFU;
        hash *= Prime;
    }

    [[nodiscard]] std::string hex() const {
        static constexpr char Digits[] = "0123456789abcdef";
        std::string text(32, '0');
        __uint128_t value = hash;
        for (std::size_t index = 32; index > 0; --index) {
            text[index - 1] = Digits[static_cast<std::size_t>(value & 0xF)];
            value >>= 4;
        }
        return text;
    }

private:
    // The published FNV-1a 128-bit parameters.
    static constexpr __uint128_t OffsetBasis =
        (static_cast<__uint128_t>(0x6c62'272e'07bb'0142ULL) << 64) | 0x62b8'2175'6295'c58dULL;
    static constexpr __uint128_t Prime =
        (static_cast<__uint128_t>(0x0000'0000'0100'0000ULL) << 64) | 0x0000'0000'0000'013bULL;

    __uint128_t hash = OffsetBasis;
};

/// Reads a whole file, or nothing.
std::optional<std::string> readWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return std::nullopt;
    return contents;
}

/// Writes `contents` to `path` through a temporary, so that an interrupted write leaves the
/// previous entry rather than a truncated one.
///
/// The rename is atomic within a directory, which is the whole point: a library half-written
/// by a build that was cancelled must still be a library.
bool writeWholeFile(const std::filesystem::path& path, std::string_view contents) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) return false;
    }

    std::error_code failure;
    std::filesystem::rename(temporary, path, failure);
    if (failure) {
        std::filesystem::remove(temporary, failure);
        return false;
    }
    return true;
}

} // namespace

bool Library::open(const std::filesystem::path& newDirectory, DiagnosticEngine& diagnostics) {
    std::error_code failure;
    if (std::filesystem::exists(newDirectory, failure)
        && !std::filesystem::is_directory(newDirectory, failure)) {
        diagnostics.report(DiagCode::LibraryNotADirectory, SourceLocation(), newDirectory.string());
        return false;
    }

    std::filesystem::create_directories(newDirectory, failure);
    if (failure) {
        diagnostics.report(DiagCode::CannotOpenFile, SourceLocation(), newDirectory.string(),
                           failure.message());
        return false;
    }

    directory = newDirectory;
    opened = true;
    return true;
}

void Library::setOptions(std::span<const std::string> defines,
                         std::span<const std::filesystem::path> includeDirectories) {
    Digest digest;

    // The format version and Enzan's own version come first, so that a build whose translator
    // changed cannot read a library written by another one that happened to agree about the
    // options.
    digest.add(std::to_string(SerializationFormatVersion));
    digest.add(versionLine());

    for (const std::string& define : defines) digest.add(define);
    for (const std::filesystem::path& include : includeDirectories) digest.add(include.string());

    optionsDigest = digest.hex();
}

std::filesystem::path Library::entryPath(const LibrarySource& source,
                                         const SourceManager& sourceManager) const {
    Digest digest;
    digest.add(optionsDigest);

    // The path as well as the text: two files with identical contents are still two units, and
    // a diagnostic about one must name the right file.
    digest.add(source.path.string());
    digest.add(sourceManager.bufferText(source.buffer));

    return directory / (digest.hex() + ".eirlib");
}

bool Library::load(std::span<const LibrarySource> sources, const SourceManager& sourceManager,
                   Design& design) {
    loadedCount = 0;
    if (!opened || sources.empty()) return false;

    // Read every entry before anything is added to the design, so that a miss on the last file
    // leaves the caller with the empty design it started with rather than a partial one.
    std::vector<std::string> payloads;
    payloads.reserve(sources.size());
    for (const LibrarySource& source : sources) {
        std::optional<std::string> contents = readWholeFile(entryPath(source, sourceManager));
        if (!contents.has_value()) return false;
        payloads.push_back(std::move(*contents));
    }

    // A stored location may name a file the caller has not read — an include, or a source from
    // an earlier analysis. Only the buffers already in hand are offered: reading files to
    // resolve a location would undo the saving the library exists for, and a location that
    // cannot be resolved simply loses its snippet.
    std::map<std::string, BufferId, std::less<>> known;
    for (const LibrarySource& source : sources)
        known.emplace(std::string(sourceManager.bufferName(source.buffer)), source.buffer);

    const auto resolve = [&known](std::string_view name) -> BufferId {
        const auto found = known.find(name);
        return found == known.end() ? BufferId() : found->second;
    };

    std::vector<ModuleDefinition> decoded;
    for (const std::string& payload : payloads)
        if (!decodeModules(payload, resolve, decoded)) return false;

    for (ModuleDefinition& definition : decoded) design.addModule(std::move(definition));
    loadedCount = sources.size();
    return true;
}

bool Library::store(std::span<const LibrarySource> sources, const SourceManager& sourceManager,
                    const Design& design, DiagnosticEngine& diagnostics) {
    if (!opened) return false;

    // Units grouped by the *name* of the buffer their definition was written in, which is how
    // a design that analysis produced in one piece is put back into per-file entries.
    //
    // By name and not by identifier, because one file legitimately has more than one buffer in
    // a run: the compile-order pre-scan reads every source and so does the frontend, and the
    // manager does not deduplicate (see SourceManager::readFile). Matching identifiers would
    // silently attribute nothing to anything and write a library of empty entries — which
    // loads perfectly and elaborates to a design with no modules in it.
    std::map<std::string, std::vector<const ModuleDefinition*>, std::less<>> byName;
    for (const ModuleDefinition& definition : design.modules()) {
        if (!definition.location.valid()) continue;
        byName[std::string(sourceManager.bufferName(definition.location.buffer()))].push_back(
            &definition);
    }

    bool allWritten = true;
    for (const LibrarySource& source : sources) {
        std::vector<ModuleDefinition> units;
        const auto found = byName.find(sourceManager.bufferName(source.buffer));
        if (found != byName.end()) {
            units.reserve(found->second.size());
            for (const ModuleDefinition* definition : found->second) units.push_back(*definition);
        }

        // A file that defines nothing still gets an entry. Without one it would look like a
        // miss for ever, and a header of macros or a file of only `` `include `` directives is
        // an ordinary thing for a design to contain.
        const std::string payload = encodeModules(units, sourceManager);
        const std::filesystem::path target = entryPath(source, sourceManager);
        if (writeWholeFile(target, payload)) continue;

        diagnostics.report(DiagCode::LibraryWriteFailed, SourceLocation(), target.string());
        allWritten = false;
    }

    return allWritten;
}

} // namespace enzan::eir
