// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/VcdWriter.h"

#include "common/Version.h"
#include "eir/BitVector.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <ostream>
#include <utility>

namespace enzan::wavedb {

struct VcdWriter::Stream {
    std::ofstream file;
};

namespace {

/// Returns the scope path of a hierarchical name: everything before the last `/`.
std::string_view scopeOf(std::string_view path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? std::string_view() : path.substr(0, separator);
}

/// Returns the leaf name of a hierarchical path.
std::string_view leafOf(std::string_view path) {
    const std::size_t separator = path.rfind('/');
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

/// True when `path` is `prefix` itself or lies inside it.
///
/// A plain `starts_with` is wrong here: it would put `/tb/u_dut2` inside `/tb/u_dut`,
/// which nests the scopes incorrectly and, for the capture filter, records a sibling
/// nobody asked for. The separator has to be part of the comparison.
bool isWithinScope(std::string_view path, std::string_view prefix) {
    if (prefix.empty() || path == prefix) return true;
    return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/';
}

} // namespace

VcdWriter::VcdWriter(const runtime::Image& designImage, const runtime::Arena& stateArena,
                     runtime::TimeScale scale)
    : image(&designImage),
      arena(&stateArena),
      timeScale(scale),
      stream(std::make_unique<Stream>()) {}

VcdWriter::~VcdWriter() {
    close();
}

void VcdWriter::setScope(std::string_view pathPrefix) {
    scopePrefix = pathPrefix;
}

std::string VcdWriter::identifierFor(std::size_t index) {
    // VCD identifiers are strings of printable ASCII from '!' (33) to '~' (126), which
    // gives 94 symbols. A base-94 encoding keeps them one character for the first 94
    // signals and two for the next nine thousand, which is what makes a VCD file for a
    // large design mostly signal values rather than mostly identifiers.
    constexpr std::size_t Base = 94;
    constexpr char First = '!';

    std::string result;
    std::size_t remaining = index;
    do {
        result.push_back(static_cast<char>(First + (remaining % Base)));
        remaining /= Base;
    } while (remaining > 0);

    return result;
}

bool VcdWriter::open(const std::filesystem::path& path, std::string& error) {
    stream->file.open(path, std::ios::out | std::ios::trunc);
    if (!stream->file.is_open()) {
        error = "could not be opened for writing";
        return false;
    }

    recordIndex.assign(image->signals().size(), NotRecorded);
    for (std::uint32_t signalIndex = 0; signalIndex < image->signals().size(); ++signalIndex) {
        const runtime::Signal& signal = image->signals()[signalIndex];
        if (!isWithinScope(signal.path, scopePrefix)) continue;

        recordIndex[signalIndex] = static_cast<std::uint32_t>(recorded.size());
        identifiers.push_back(identifierFor(recorded.size()));
        recorded.push_back(signalIndex);
    }

    isDirty.assign(recorded.size(), false);
    dirty.reserve(recorded.size());

    writeDeclarations();

    // The initial values are a full dump at time zero, so that a viewer shows the state
    // the design starts from rather than leaving every trace undefined until its first
    // change.
    stream->file << "#0\n$dumpvars\n";
    for (const std::uint32_t signalIndex : recorded) writeValue(signalIndex);
    stream->file << "$end\n";

    pendingTime = 0;
    headerWritten = true;
    return true;
}

void VcdWriter::writeDeclarations() {
    stream->file << "$version Enzan " << enzan::version() << " $end\n";
    stream->file << "$timescale 1" << timeScale.unitName() << " $end\n";

    // Every name a signal has is declared, all sharing one identifier. A collapsed port
    // gives one slot two hierarchical names, and both are real: the user may look for
    // either, and a viewer showing a DUT's port only under the parent's name is
    // confusing. VCD supports several $var declarations with the same identifier exactly
    // for this.
    struct Entry {
        std::uint32_t signalIndex;
        std::string leafName;
    };

    std::map<std::string, std::vector<Entry>, std::less<>> byScope;
    for (const std::uint32_t signalIndex : recorded) {
        for (const std::string& name : image->elaboratedDesign().slotNames(signalIndex)) {
            byScope[std::string(scopeOf(name))].push_back(
                Entry{.signalIndex = signalIndex, .leafName = std::string(leafOf(name))});
        }
    }

    // Instances are emitted in elaboration order, which is a depth-first walk of the
    // hierarchy, so the scope nesting can be produced by comparing each path with the
    // previous one rather than by recursing.
    std::vector<std::string_view> openScopes;
    const auto closeTo = [&](std::string_view path) {
        while (!openScopes.empty() && !isWithinScope(path, openScopes.back())) {
            stream->file << "$upscope $end\n";
            openScopes.pop_back();
        }
    };

    for (const elab::Instance& instance : image->elaboratedDesign().instances()) {
        closeTo(instance.path);

        stream->file << "$scope module " << instance.name << " $end\n";
        openScopes.push_back(instance.path);

        const auto entries = byScope.find(instance.path);
        if (entries != byScope.end()) {
            for (const Entry& entry : entries->second) {
                const runtime::Signal& signal = image->signals()[entry.signalIndex];
                // `wire` for everything: GTKWave and Surfer do not distinguish, and the
                // net/variable distinction that does matter belongs to driver resolution
                // (M5) rather than to the waveform.
                stream->file << "$var wire " << signal.bitWidth << ' '
                             << identifiers[recordIndex[entry.signalIndex]] << ' ' << entry.leafName
                             << " $end\n";
            }
        }
    }

    while (!openScopes.empty()) {
        stream->file << "$upscope $end\n";
        openScopes.pop_back();
    }

    stream->file << "$enddefinitions $end\n";
}

void VcdWriter::writeValue(std::uint32_t signalIndex) {
    const runtime::Signal& signal = image->signals()[signalIndex];
    const eir::ConstBitRef value = arena->at(signal.wordOffset, signal.bitWidth);
    const std::string& identifier = identifiers[recordIndex[signalIndex]];

    if (signal.bitWidth == 1) {
        stream->file << eir::toChar(value.get(0)) << identifier << '\n';
        return;
    }

    // Vectors are emitted at full width. The format permits dropping leading zeroes, but
    // a constant width makes a diff of two VCD files line up, which is what section 14.3's
    // differential testing needs.
    stream->file << 'b' << eir::toString(value, eir::Radix::Binary) << ' ' << identifier << '\n';
}

void VcdWriter::onValueChange(std::uint32_t signalIndex, eir::ConstBitRef /*value*/) {
    if (!headerWritten || signalIndex >= recordIndex.size()) return;

    const std::uint32_t index = recordIndex[signalIndex];
    if (index == NotRecorded || isDirty[index]) return;

    // Only the fact of the change is recorded. The value is read from the arena when the
    // time slot is flushed, so several changes within one slot collapse into the value the
    // slot settles on.
    isDirty[index] = true;
    dirty.push_back(signalIndex);
}

void VcdWriter::onTimeAdvance(runtime::SimTime time) {
    // The changes accumulated so far belong to the time slot that is ending, not to the
    // one about to begin.
    flushPendingTime();
    pendingTime = time;
}

void VcdWriter::flushPendingTime() {
    if (!headerWritten || dirty.empty()) return;

    // Time zero's values were already dumped by `open`, so a change at time zero appends
    // to that timestamp rather than repeating it.
    if (pendingTime != 0) stream->file << '#' << pendingTime << '\n';

    for (const std::uint32_t signalIndex : dirty) {
        writeValue(signalIndex);
        isDirty[recordIndex[signalIndex]] = false;
    }
    dirty.clear();
}

bool VcdWriter::isOpen() const noexcept {
    return stream != nullptr && stream->file.is_open();
}

void VcdWriter::close() {
    if (!isOpen()) return;

    flushPendingTime();
    stream->file.flush();
    stream->file.close();
}

} // namespace enzan::wavedb
