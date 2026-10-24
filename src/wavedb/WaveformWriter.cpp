// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/WaveformWriter.h"

#include "eir/BitOps.h"
#include "eir/Module.h"

#include "elab/ElaboratedDesign.h"

#include <algorithm>
#include <format>
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

std::uint32_t WaveformWriter::widthOf(std::uint32_t signalIndex) const noexcept {
    // Its one bit is a placeholder, a bit vector having at least one, which is no value of it.
    if (designImage->elaboratedDesign().slots()[signalIndex].holdsNothing) return 0;
    const runtime::Signal& signal = designImage->signals()[signalIndex];
    return eir::presentedWidth(signal.encoding, signal.bitWidth);
}

std::string WaveformWriter::describeTooWide() const {
    constexpr std::size_t Named = 3;
    const std::size_t named = std::min(leftOutForWidth.size(), Named);

    std::string names;
    for (std::size_t index = 0; index < named; ++index) {
        if (index > 0) names += index + 1 == named ? " and " : ", ";
        const std::uint32_t signalIndex = leftOutForWidth[index];
        names += std::format("{} of {} bits", designImage->signals()[signalIndex].path,
                             widthOf(signalIndex));
    }

    if (leftOutForWidth.size() <= 1) return names;
    if (leftOutForWidth.size() <= Named)
        return std::format("{} signals, {}", leftOutForWidth.size(), names);
    return std::format("{} signals, among them {}", leftOutForWidth.size(), names);
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
    leftOutForWidth.clear();

    // A signal is recorded under the names the reader wrote, and one that has none, being only
    // storage a frontend introduced, is not recorded at all (eir::isIntroducedName). One pass over
    // every name rather than a search per signal, which would be quadratic in the design.
    std::vector<bool> named(designImage->signals().size(), false);
    for (const auto& [path, slotIndex] : designImage->elaboratedDesign().namesByPath()) {
        if (slotIndex < named.size() && !eir::isIntroducedName(leafOf(path)))
            named[slotIndex] = true;
    }

    for (std::uint32_t signalIndex = 0; signalIndex < designImage->signals().size();
         ++signalIndex) {
        if (!named[signalIndex]) continue;
        if (!isRecorded(designImage->signals()[signalIndex].path)) continue;

        // A file records no memory, as IEEE 1364 section 18 leaves memories out of a VCD.
        const elab::Slot& slot = designImage->elaboratedDesign().slots()[signalIndex];
        if (!slot.unpackedDimensions.empty() && !recordsArrays()) continue;

        // Nor is a VHDL signal of a null range recorded, whose one bit is no value of it.
        if (slot.holdsNothing) continue;

        // Nor one wider than the limit, whose every change would cost the whole of it.
        if (widthLimit != 0 && widthOf(signalIndex) > widthLimit) {
            leftOutForWidth.push_back(signalIndex);
            continue;
        }

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
    // a DUT's port only under its parent's name is confusing. A name a frontend made up is no
    // name of the reader's, and is left out.
    std::map<std::string, std::vector<Entry>, std::less<>> byScope;
    for (const std::uint32_t signalIndex : recorded) {
        for (const std::string& name : designImage->elaboratedDesign().slotNames(signalIndex)) {
            if (eir::isIntroducedName(leafOf(name))) continue;
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
