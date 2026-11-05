// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Module.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {
class SourceManager;
}

namespace enzan::eir {

/// The version of the serialised EIR format.
///
/// **Bump this whenever the bytes change meaning**, which is not only when a field is added
/// or removed. It also covers a change to how the frontend *translates* source into EIR: a
/// stored unit produced by an older translator is as wrong as one whose fields moved, and it
/// is far harder to notice because it decodes cleanly and simulates to a different answer.
/// A version bump costs one re-analysis.
///
/// A mismatch makes the reader refuse the bytes, which makes the library re-analyse
/// (DESIGN_GUIDE.md section 4.4). It never crashes and never half-decodes.
constexpr std::uint32_t SerializationFormatVersion = 71;

/// Encodes design units into a self-describing byte string.
///
/// Source locations are stored as a buffer *name* plus an offset rather than as a BufferId,
/// because identifiers are assigned in the order buffers happen to be read and mean nothing in
/// the next process. The names are collected into a table at the front of the encoding, so a
/// unit whose locations all come from one file pays for that path once.
///
/// The encoding is little-endian throughout and contains no padding, pointers or sizes of
/// host types, so a library written by one build reads in another of the same format version.
[[nodiscard]] std::string encodeModules(std::span<const ModuleDefinition> modules,
                                        const SourceManager& sources);

/// Decodes what `encodeModules` produced, appending to `modules`.
///
/// `resolveBuffer` maps a stored buffer name onto a buffer in the caller's source manager.
/// Returning an invalid identifier is not an error: it leaves those locations invalid, which
/// costs a diagnostic its snippet and nothing else. That matters because a library may name
/// an include file that has since been deleted, and refusing to load a whole design over a
/// missing snippet would be a poor trade.
///
/// Returns false, having written nothing, when the bytes are not a library of this format
/// version, are truncated, or fail their checksum. Every one of those means "analyse it
/// again" and none of them means "carry on with what was decoded so far": a mismatch causes
/// clean re-analysis rather than silent misbehaviour.
[[nodiscard]] bool decodeModules(std::string_view bytes,
                                 const std::function<BufferId(std::string_view)>& resolveBuffer,
                                 std::vector<ModuleDefinition>& modules);

/// Encodes what `definition` is, leaving out where it came from.
///
/// Two copies of one unit agree on this byte for byte however each reached the caller: one a
/// session analysed, with its source locations, and one an input library decoded, without
/// them. Anything else that differs — a width, a statement, a name — makes the bytes differ,
/// so equal bytes are the same unit and not merely the same name.
///
/// The locations are left out by the route a library read takes: the unit is encoded, decoded
/// without resolving a buffer, and encoded again, so nothing has to list where a unit keeps a
/// location for this to stay right when a new one is added.
[[nodiscard]] std::string encodeContent(const ModuleDefinition& definition,
                                        const SourceManager& sources);

} // namespace enzan::eir
