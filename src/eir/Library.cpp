// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Library.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "common/Version.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Serialization.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <set>
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

/// The first line of a library's index, so that a directory of unrelated files is recognised
/// as not being a library rather than decoded as a broken one.
constexpr std::string_view IndexMagic = "enzan-library";

/// The index's own version, separate from the serialised unit format: the two change for
/// different reasons, and a reader must be able to say which of them it does not understand.
constexpr int IndexVersion = 2;

constexpr std::string_view IndexFileName = "enzan.lib";

/// Prefixes an entry, ahead of its staleness key and the units themselves.
constexpr std::string_view EntryMagic = "enzan-entry\n";

/// Characters in a key, which is a 128-bit digest in hex.
constexpr std::size_t DigestHexLength = 32;

/// One entry's header: the files it depended on, and where its units begin.
struct EntryHeader {
    std::vector<std::filesystem::path> dependencies;

    /// Offset of the key. The units follow it.
    std::size_t keyOffset = 0;
};

/// Parses an entry's header, or nothing when it is not one this build wrote.
///
/// One reader for every caller — the cache check, a library being resolved against, and a
/// listing — because three parsers of one format is three chances for them to disagree about
/// where the units start.
std::optional<EntryHeader> readEntryHeader(std::string_view contents) {
    if (!contents.starts_with(EntryMagic)) return std::nullopt;

    EntryHeader header;
    std::size_t cursor = EntryMagic.size();
    for (;;) {
        const std::size_t end = contents.find('\n', cursor);
        if (end == std::string_view::npos) return std::nullopt;

        const std::string_view line = contents.substr(cursor, end - cursor);
        cursor = end + 1;
        if (line.empty()) break;
        header.dependencies.emplace_back(line);
    }

    if (contents.size() < cursor + DigestHexLength) return std::nullopt;
    header.keyOffset = cursor;
    return header;
}

} // namespace

bool Library::isValidName(std::string_view text) noexcept {
    if (text.empty()) return false;

    const auto isLetter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!isLetter(text.front())) return false;

    return std::ranges::all_of(text, [&](char c) { return isLetter(c) || (c >= '0' && c <= '9'); });
}

bool Library::create(const std::filesystem::path& newDirectory, std::string_view name,
                     DiagnosticEngine& diagnostics) {
    std::error_code failure;
    if (std::filesystem::exists(newDirectory, failure)
        && !std::filesystem::is_directory(newDirectory, failure)) {
        diagnostics.report(DiagCode::LibraryNotADirectory, SourceLocation(), newDirectory.string());
        return false;
    }

    // An empty name means the directory's own, which is what `--out-lib libs/uart` means
    // without being made to say `uart` twice.
    std::string wanted(name);
    if (wanted.empty()) wanted = newDirectory.filename().string();

    if (!isValidName(wanted)) {
        diagnostics.report(DiagCode::LibraryBadName, SourceLocation(), wanted);
        return false;
    }

    std::filesystem::create_directories(newDirectory, failure);
    if (failure) {
        diagnostics.report(DiagCode::CannotOpenFile, SourceLocation(), newDirectory.string(),
                           failure.message());
        return false;
    }

    directory = newDirectory;
    logicalName = std::move(wanted);
    recordedSources.clear();
    opened = true;
    return writeIndex(diagnostics);
}

bool Library::writeIndex(DiagnosticEngine& diagnostics) const {
    std::string index(IndexMagic);
    index += '\n';
    index += std::to_string(IndexVersion);
    index += '\n';
    index += logicalName;
    index += '\n';

    // The sources this library was analysed from *and the options they need*, so that a design
    // analysed against it can read the declarations it wants (8.2). A source is no use without
    // the include path that resolves its `` `include `` directives and the macros it was
    // compiled with: reading it without them fails, or worse parses to something else.
    //
    // Paths and not text: duplicating the RTL into every library that uses it would make a
    // library a copy of the design rather than an index of it.
    //
    // Tagged rather than positional so that a later field is an addition and not a new format.
    for (const std::filesystem::path& source : recordedSources) {
        index += "source ";
        index += source.string();
        index += '\n';
    }
    for (const std::filesystem::path& include : recordedIncludeDirectories) {
        index += "incdir ";
        index += include.string();
        index += '\n';
    }
    for (const std::string& define : recordedDefines) {
        index += "define ";
        index += define;
        index += '\n';
    }
    if (recordedTimeUnit.has_value()) {
        index += "timeunit ";
        index += std::to_string(*recordedTimeUnit);
        index += '\n';
    }

    const std::filesystem::path indexPath = directory / IndexFileName;
    if (writeWholeFile(indexPath, index)) return true;

    diagnostics.report(DiagCode::CannotOpenFile, SourceLocation(), indexPath.string(),
                       "the library index could not be written");
    return false;
}

bool Library::open(const std::filesystem::path& newDirectory, DiagnosticEngine& diagnostics) {
    const std::filesystem::path indexPath = newDirectory / IndexFileName;
    const std::optional<std::string> index = readWholeFile(indexPath);
    if (!index.has_value()) {
        // A directory that merely exists is not a library. Reported rather than created,
        // because a library nobody asked for is a cache nobody asked for (4.4).
        diagnostics.report(DiagCode::NotALibrary, SourceLocation(), newDirectory.string());
        return false;
    }

    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < index->size();) {
        const std::size_t end = std::min(index->find('\n', start), index->size());
        lines.push_back(std::string_view(*index).substr(start, end - start));
        start = end + 1;
    }

    if (lines.size() < 3 || lines[0] != IndexMagic) {
        diagnostics.report(DiagCode::NotALibrary, SourceLocation(), newDirectory.string());
        return false;
    }
    if (lines[1] != std::to_string(IndexVersion)) {
        diagnostics.report(DiagCode::LibraryVersionMismatch, SourceLocation(),
                           newDirectory.string(), std::string(lines[1]),
                           std::to_string(IndexVersion));
        return false;
    }
    if (!isValidName(lines[2])) {
        diagnostics.report(DiagCode::LibraryBadName, SourceLocation(), std::string(lines[2]));
        return false;
    }

    directory = newDirectory;
    logicalName = std::string(lines[2]);

    recordedSources.clear();
    recordedIncludeDirectories.clear();
    recordedDefines.clear();
    recordedTimeUnit.reset();
    for (std::size_t line = 3; line < lines.size(); ++line) {
        const std::string_view text = lines[line];
        if (text.starts_with("source ")) {
            recordedSources.emplace_back(text.substr(7));
        } else if (text.starts_with("incdir ")) {
            recordedIncludeDirectories.emplace_back(text.substr(7));
        } else if (text.starts_with("define ")) {
            recordedDefines.emplace_back(text.substr(7));
        } else if (text.starts_with("timeunit ")) {
            int exponent = 0;
            const std::string_view value = text.substr(9);
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), exponent);
            if (error != std::errc() || end != value.data() + value.size() || exponent < -128
                || exponent > 127) {
                diagnostics.report(DiagCode::NotALibrary, SourceLocation(), newDirectory.string());
                return false;
            }
            recordedTimeUnit = static_cast<std::int8_t>(exponent);
        }
    }

    opened = true;
    return true;
}

void Library::setOptions(std::span<const std::string> defines,
                         std::span<const std::filesystem::path> includeDirectories,
                         std::span<const std::string> parameters, std::int8_t timeUnitExponent) {
    // Kept as well as hashed: the hash says whether an entry is current, and these say how to
    // read the sources again from somewhere else.
    recordedDefines.assign(defines.begin(), defines.end());
    recordedIncludeDirectories.assign(includeDirectories.begin(), includeDirectories.end());

    Digest digest;

    // The format version and Enzan's own version come first, so that a build whose translator
    // changed cannot read a library written by another one that happened to agree about the
    // options.
    digest.add(std::to_string(SerializationFormatVersion));
    digest.add(versionLine());

    for (const std::string& define : defines) digest.add(define);
    for (const std::filesystem::path& include : includeDirectories) digest.add(include.string());
    // Marked, so that an override cannot hash the same as a define spelt like it.
    for (const std::string& parameter : parameters) digest.add("-G" + parameter);
    digest.add("timeunit " + std::to_string(timeUnitExponent));
    pendingTimeUnit = timeUnitExponent;

    optionsDigest = digest.hex();
}

std::filesystem::path Library::entryPath(const LibrarySource& source) const {
    // Everything that is not a letter, a digit, a dash or a dot becomes an underscore, so the
    // result is a filename on any filesystem and still readable as the source it came from.
    std::string readable = source.path.filename().string();
    for (char& c : readable) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                          || c == '-' || c == '.';
        if (!keep) c = '_';
    }

    // Of the path and never of the text: two sources may share a basename, and an entry whose
    // name moved when its file was edited would leave the old one behind on every save.
    Digest digest;
    digest.add(source.path.string());

    return directory / (readable + "-" + digest.hex().substr(0, 8) + ".eirlib");
}

std::string Library::entryKey(const LibrarySource& source, const SourceManager& sourceManager,
                              std::span<const std::filesystem::path> dependencies) {
    Digest digest;
    digest.add(optionsDigest);
    digest.add(source.path.string());
    digest.add(sourceManager.bufferText(source.buffer));

    // The dependencies' *contents*, so that editing an included header makes this entry stale.
    // In the order given, which the caller sorts, because a digest is order-dependent and
    // section 3.8 keeps observable results off the order a directory happened to be read in.
    for (const std::filesystem::path& dependency : dependencies) {
        digest.add(dependency.string());
        const std::optional<std::string> contents = readWholeFile(dependency);
        digest.add(contents.value_or(std::string("<unreadable>")));
    }
    return digest.hex();
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
        const std::optional<std::string> contents = readWholeFile(entryPath(source));
        if (!contents.has_value() || !contents->starts_with(EntryMagic)) return false;

        // The entry names the files it depended on, so that their contents can be folded into
        // the key it is checked against. Without that a changed header leaves the entry looking
        // current and the library serves EIR the source no longer produces.
        const std::optional<EntryHeader> header = readEntryHeader(*contents);
        if (!header.has_value()) return false;

        // The key is inside the entry rather than in its name (4.4), so a stale entry is one
        // that is present and disagrees rather than one that is missing.
        const std::string wanted = entryKey(source, sourceManager, header->dependencies);
        if (contents->compare(header->keyOffset, wanted.size(), wanted) != 0) return false;

        payloads.push_back(contents->substr(header->keyOffset + wanted.size()));
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

    // Stamped on the way in rather than stored in the entry, so that the name a design refers
    // to a unit by is the one the library carries and cannot disagree with it (8.2).
    for (ModuleDefinition& definition : decoded) {
        definition.library = logicalName;
        design.addModule(std::move(definition));
    }
    loadedCount = sources.size();
    return true;
}

bool Library::store(std::span<const LibrarySource> sources, const SourceManager& sourceManager,
                    const Design& design, DiagnosticEngine& diagnostics,
                    const std::set<std::string, std::less<>>& excluded,
                    std::span<const std::filesystem::path> dependencies) {
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
        if (excluded.contains(definition.name)) continue;
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
        std::string payload(EntryMagic);
        for (const std::filesystem::path& dependency : dependencies) {
            payload += dependency.string();
            payload += '\n';
        }
        payload += '\n';
        payload += entryKey(source, sourceManager, dependencies);
        payload += encodeModules(units, sourceManager);
        const std::filesystem::path target = entryPath(source);
        if (writeWholeFile(target, payload)) continue;

        diagnostics.report(DiagCode::LibraryWriteFailed, SourceLocation(), target.string());
        allWritten = false;
    }

    // Recorded so that a design analysed against this library can read these declarations, and
    // run in the unit they were analysed in. Replaced rather than merged: the sources named now
    // are what the library holds now.
    recordedTimeUnit = pendingTimeUnit;
    recordedSources.clear();
    for (const LibrarySource& source : sources) recordedSources.push_back(source.path);
    if (!writeIndex(diagnostics)) allWritten = false;

    return allWritten;
}

std::optional<std::int8_t> Library::timeUnit() const {
    // A library emptied by `lib clean` keeps its index, and so the unit it last held; it holds
    // no units in it any more, so it can be filled in another.
    std::error_code failure;
    for (const auto& entry : std::filesystem::directory_iterator(directory, failure)) {
        if (entry.path().extension() == ".eirlib") return recordedTimeUnit;
    }
    return std::nullopt;
}

bool Library::overrideName(std::string_view replacement) {
    if (!isValidName(replacement)) return false;
    logicalName = std::string(replacement);
    return true;
}

bool Library::loadAll(Design& design, DiagnosticEngine& diagnostics) const {
    if (!opened) return false;

    // Gathered and sorted before anything is decoded: a directory listing arrives in whatever
    // order the filesystem gives, and section 3.8 puts observable ordering beyond that.
    std::vector<std::filesystem::path> entries;
    std::error_code failure;
    for (const auto& entry : std::filesystem::directory_iterator(directory, failure)) {
        if (failure) break;
        if (entry.path().extension() == ".eirlib") entries.push_back(entry.path());
    }
    std::ranges::sort(entries);

    for (const std::filesystem::path& entry : entries) {
        const std::optional<std::string> contents = readWholeFile(entry);
        if (!contents.has_value()) {
            diagnostics.report(DiagCode::LibraryEntryUnreadable, SourceLocation(), entry.string());
            return false;
        }

        const std::optional<EntryHeader> header = readEntryHeader(*contents);
        if (!header.has_value()) {
            diagnostics.report(DiagCode::LibraryEntryUnreadable, SourceLocation(), entry.string());
            return false;
        }
        const std::size_t offset = header->keyOffset + DigestHexLength;

        // Locations name buffers this process has not read; a unit from a library it is only
        // resolving against carries no snippet, which costs a diagnostic its source line and
        // nothing else.
        const auto noBuffers = [](std::string_view) { return BufferId(); };

        std::vector<ModuleDefinition> decoded;
        if (!decodeModules(std::string_view(*contents).substr(offset), noBuffers, decoded)) {
            diagnostics.report(DiagCode::LibraryEntryUnreadable, SourceLocation(), entry.string());
            return false;
        }

        for (ModuleDefinition& definition : decoded) {
            definition.library = logicalName;
            design.addModule(std::move(definition));
        }
    }
    return true;
}

std::vector<Library::UnitSummary> Library::units() const {
    std::vector<UnitSummary> summaries;
    if (!opened) return summaries;

    // Collected into an ordered set rather than reported in directory order: a directory
    // listing is whatever the filesystem feels like, and section 3.8 puts observable ordering
    // beyond the reach of that.
    std::map<std::string, UnitSummary, std::less<>> sorted;

    std::error_code failure;
    for (const auto& entry : std::filesystem::directory_iterator(directory, failure)) {
        if (failure) break;
        if (entry.path().extension() != ".eirlib") continue;

        const std::optional<std::string> contents = readWholeFile(entry.path());
        if (!contents.has_value()) continue;

        const std::optional<EntryHeader> header = readEntryHeader(*contents);
        if (!header.has_value()) continue;

        const std::size_t offset = header->keyOffset + DigestHexLength;

        // Nothing here has the buffers a location names, and a listing does not need them.
        const auto noBuffers = [](std::string_view) { return BufferId(); };

        std::vector<ModuleDefinition> decoded;
        if (!decodeModules(std::string_view(*contents).substr(offset), noBuffers, decoded))
            continue;

        for (const ModuleDefinition& definition : decoded) {
            // A parameterisation is named for its values (`specialisedName`), and only it is:
            // a type parameter has no value to record, so the name is what tells.
            const bool parameterised = definition.name != definition.sourceName;
            sorted.insert_or_assign(
                definition.name,
                UnitSummary{.name = definition.name,
                            .sourceName = definition.sourceName,
                            .generic = definition.isGeneric(),
                            .specialised = parameterised && !definition.isGeneric()});
        }
    }

    for (auto& [name, summary] : sorted) summaries.push_back(std::move(summary));
    return summaries;
}

std::vector<std::string> Library::unitNames() const {
    std::vector<std::string> names;
    for (UnitSummary& summary : units()) names.push_back(std::move(summary.name));
    return names;
}

} // namespace enzan::eir
