// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/WaveformWriter.h"

#include "eir/BitOps.h"

#include "elab/ElaboratedDesign.h"

#include <algorithm>
#include <map>

namespace enzan::wavedb {

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
/// A plain `starts_with` is wrong here: it would put `/tb/u_dut2` inside `/tb/u_dut`, which nests
/// the scopes incorrectly and, for the capture filter, records a sibling nobody asked for. The
/// separator has to be part of the comparison.
bool isWithinScope(std::string_view path, std::string_view prefix) {
    if (prefix.empty() || path == prefix) return true;
    return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/';
}

} // namespace

WaveformWriter::WaveformWriter(const runtime::Image& designImageToRecord,
                               const runtime::Arena& arena, runtime::TimeScale timeScaleToUse)
    : designImage(&designImageToRecord), stateArena(&arena), scale(timeScaleToUse) {}

WaveformWriter::~WaveformWriter() = default;

void WaveformWriter::setScope(std::string_view pathPrefix) {
    scopePrefixes.clear();
    addScope(pathPrefix);
}

void WaveformWriter::addScope(std::string_view pathPrefix) {
    // An empty prefix means everything, so it subsumes whatever else was asked for and there
    // is no point carrying the rest.
    if (pathPrefix.empty()) {
        scopePrefixes.clear();
        return;
    }

    if (std::ranges::find(scopePrefixes, pathPrefix) != scopePrefixes.end()) return;
    scopePrefixes.emplace_back(pathPrefix);
}

bool WaveformWriter::isRecorded(std::string_view path) const {
    // No prefix at all records the whole design; otherwise any one of them is enough. Once a
    // prefix is empty `addScope` has already cleared the list, so this needs no empty case.
    if (scopePrefixes.empty()) return true;

    return std::ranges::any_of(
        scopePrefixes, [path](const std::string& prefix) { return isWithinScope(path, prefix); });
}

std::uint32_t WaveformWriter::recordIndexOf(std::uint32_t signalIndex) const noexcept {
    return signalIndex < recordIndex.size() ? recordIndex[signalIndex] : NotRecorded;
}

eir::ConstBitRef WaveformWriter::valueOf(std::uint32_t signalIndex) const {
    const runtime::Signal& signal = designImage->signals()[signalIndex];
    const eir::ConstBitRef stored = stateArena->at(signal.wordOffset, signal.bitWidth);
    if (signal.encoding == eir::Encoding::Bits) return stored;

    const std::uint32_t width = eir::presentedWidth(signal.encoding, signal.bitWidth);
    if (shown.width() != width) shown = eir::BitVector(width);
    eir::present(signal.encoding, stored, shown.mutableRef());
    return shown.ref();
}

double WaveformWriter::realValueOf(std::uint32_t signalIndex) const {
    const runtime::Signal& signal = designImage->signals()[signalIndex];
    return eir::bitops::realOf(stateArena->at(signal.wordOffset, signal.bitWidth));
}

void WaveformWriter::selectSignals() {
    recordIndex.assign(designImage->signals().size(), NotRecorded);

    for (std::uint32_t signalIndex = 0; signalIndex < designImage->signals().size();
         ++signalIndex) {
        if (!isRecorded(designImage->signals()[signalIndex].path)) continue;

        // A memory is not recorded, as IEEE 1364 section 18 leaves memories out of a VCD: one
        // of any size would dominate the file and the database with a value nobody reads as a
        // waveform.
        if (!designImage->elaboratedDesign().slots()[signalIndex].unpackedDimensions.empty())
            continue;

        recordIndex[signalIndex] = static_cast<std::uint32_t>(recorded.size());
        recorded.push_back(signalIndex);
    }

    isDirty.assign(recorded.size(), false);
    dirty.reserve(recorded.size());
}

void WaveformWriter::declareHierarchy() {
    struct Entry {
        std::uint32_t signalIndex;
        std::string leafName;
    };

    // Every name a recorded signal has, grouped by the scope it appears in. A collapsed port has
    // two names for one slot and both are declared, because both are real and a viewer that shows
    // a DUT's port only under its parent's name is confusing.
    std::map<std::string, std::vector<Entry>, std::less<>> byScope;
    for (const std::uint32_t signalIndex : recorded) {
        for (const std::string& name : designImage->elaboratedDesign().slotNames(signalIndex)) {
            byScope[std::string(scopeOf(name))].push_back(
                Entry{.signalIndex = signalIndex, .leafName = std::string(leafOf(name))});
        }
    }

    // Instances come in elaboration order, which is a depth-first walk, so the nesting follows
    // from comparing each path with the scopes still open rather than from recursing.
    std::vector<std::string_view> openScopes;
    for (const elab::Instance& instance : designImage->elaboratedDesign().instances()) {
        while (!openScopes.empty() && !isWithinScope(instance.path, openScopes.back())) {
            leaveScope();
            openScopes.pop_back();
        }

        enterScope(instance.name);
        openScopes.push_back(instance.path);

        const auto entries = byScope.find(instance.path);
        if (entries == byScope.end()) continue;

        for (const Entry& entry : entries->second) declareSignal(entry.signalIndex, entry.leafName);
    }

    while (!openScopes.empty()) {
        leaveScope();
        openScopes.pop_back();
    }
}

bool WaveformWriter::open(const std::filesystem::path& path, std::string& error) {
    if (!openOutput(path, error)) return false;

    selectSignals();

    writeHeader();
    declareHierarchy();
    endDeclarations();

    // A full dump at time zero, so that a viewer shows the state the design starts from rather
    // than leaving every trace undefined until its first change.
    writeTimestamp(0);
    beginInitialDump();
    for (const std::uint32_t signalIndex : recorded) writeChange(signalIndex);
    endInitialDump();

    pendingTime = 0;
    opened = true;
    return true;
}

void WaveformWriter::onValueChange(std::uint32_t signalIndex, eir::ConstBitRef /*value*/) {
    if (!opened) return;

    const std::uint32_t index = recordIndexOf(signalIndex);
    if (index == NotRecorded || isDirty[index]) return;

    // Only the fact of the change is recorded. The value is read from the arena when the slot is
    // flushed, so several changes within one slot collapse into the value it settles on — which is
    // what every other simulator records, and what stops a diff of two waveforms from drowning in
    // delta-level noise (section 14.3).
    isDirty[index] = true;
    dirty.push_back(signalIndex);
}

void WaveformWriter::onTimeAdvance(runtime::SimTime time) {
    // The changes accumulated so far belong to the slot that is ending, not the one about to
    // begin. Getting this backwards shifts every trace by one timestamp, which a viewer makes
    // look like a design bug.
    flushPendingTime();
    pendingTime = time;
}

void WaveformWriter::flushPendingTime() {
    if (!opened || dirty.empty()) return;

    // Time zero's values were dumped by `open`, so a change at time zero appends to that
    // timestamp rather than repeating it.
    if (pendingTime != 0) writeTimestamp(pendingTime);

    for (const std::uint32_t signalIndex : dirty) {
        writeChange(signalIndex);
        isDirty[recordIndexOf(signalIndex)] = false;
    }
    dirty.clear();
}

void WaveformWriter::flush() {
    flushPendingTime();
}

void WaveformWriter::close() {
    if (!opened) return;

    flushPendingTime();
    opened = false;
    closeOutput();
}

} // namespace enzan::wavedb
