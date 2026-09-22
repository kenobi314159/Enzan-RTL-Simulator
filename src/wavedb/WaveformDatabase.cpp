// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/WaveformDatabase.h"

#include "common/Assert.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace enzan::wavedb {

namespace {

constexpr std::uint64_t BytesPerWord = sizeof(eir::Word);

/// Ceiling on the number of columns `sample` will summarise into.
///
/// The rounding arithmetic multiplies a remainder below the column count by a column index,
/// so the product stays inside 64 bits only while the count does not approach 2^32. A million
/// columns is already far more than any display has, so the limit costs nothing and removes
/// the overflow from consideration.
constexpr std::size_t MaxSampleColumns = 1U << 20U;

} // namespace

WaveformDatabase::WaveformDatabase(DatabaseOptions databaseOptions)
    : options(std::move(databaseOptions)) {
    ENZAN_ASSERT(options.chunkCapacity > 0, "a chunk must hold at least one change");
}

WaveformDatabase::~WaveformDatabase() {
    if (scratchFd >= 0) ::close(scratchFd);
}

std::uint32_t WaveformDatabase::addSignal(std::uint32_t signalIndex, std::string path,
                                          std::uint32_t width) {
    ENZAN_ASSERT(width > 0, "a recorded signal must have at least one bit");

    const auto existing = indexBySignal.find(signalIndex);
    if (existing != indexBySignal.end()) return existing->second;

    const auto recordIndex = static_cast<std::uint32_t>(signalList.size());

    SignalStore store;
    store.info.signalIndex = signalIndex;
    store.info.path = std::move(path);
    store.info.width = width;
    store.valueWords = eir::storageWordsFor(width);
    signalList.push_back(std::move(store));

    indexBySignal.emplace(signalIndex, recordIndex);
    return recordIndex;
}

const RecordedSignal& WaveformDatabase::signal(std::uint32_t recordIndex) const {
    return storeAt(recordIndex).info;
}

std::uint32_t WaveformDatabase::recordIndexOf(std::uint32_t signalIndex) const {
    const auto found = indexBySignal.find(signalIndex);
    return found == indexBySignal.end() ? NotRecorded : found->second;
}

WaveformDatabase::SignalStore& WaveformDatabase::storeAt(std::uint32_t recordIndex) {
    ENZAN_ASSERT(recordIndex < signalList.size(), "no such recorded signal");
    return signalList[recordIndex];
}

const WaveformDatabase::SignalStore& WaveformDatabase::storeAt(std::uint32_t recordIndex) const {
    ENZAN_ASSERT(recordIndex < signalList.size(), "no such recorded signal");
    return signalList[recordIndex];
}

std::uint64_t WaveformDatabase::chunkBytes(const SignalStore& store) const noexcept {
    return std::uint64_t{options.chunkCapacity} * store.valueWords * BytesPerWord;
}

void WaveformDatabase::append(std::uint32_t recordIndex, runtime::SimTime time,
                              eir::ConstBitRef value) {
    SignalStore& store = storeAt(recordIndex);
    ENZAN_ASSERT(value.width() == store.info.width, "a change must carry the signal's width");
    ENZAN_ASSERT(store.times.empty() || store.times.back() <= time,
                 "value changes must be appended in non-decreasing time order");

    const std::uint64_t changeIndex = store.times.size();
    const auto chunkIndex = static_cast<std::uint32_t>(changeIndex / options.chunkCapacity);

    if (chunkIndex == store.chunks.size()) store.chunks.emplace_back();

    Chunk& chunk = store.chunks[chunkIndex];
    ENZAN_ASSERT(chunk.resident, "the chunk being filled is never spilled");

    chunk.values.insert(chunk.values.end(), value.aval(), value.aval() + store.valueWords);
    residentBytes += std::uint64_t{store.valueWords} * BytesPerWord;

    store.times.push_back(time);
    store.unknownPrefix.push_back(store.unknownPrefix.back() + (value.hasUnknown() ? 1U : 0U));

    latest = std::max(time, latest);

    // Sealing the chunk is what makes it a candidate for eviction, so it is also the moment
    // the budget can first be met by spilling something.
    if (chunk.values.size() == std::size_t{options.chunkCapacity} * store.valueWords) {
        residentChunks.emplace_front(recordIndex, chunkIndex);
        chunk.lruPosition = residentChunks.begin();
        chunk.evictable = true;
        enforceBudget();
    }
}

void WaveformDatabase::setLatestTime(runtime::SimTime time) noexcept {
    latest = std::max(time, latest);
}

std::uint64_t WaveformDatabase::changeCount(std::uint32_t recordIndex) const {
    return storeAt(recordIndex).times.size();
}

runtime::SimTime WaveformDatabase::timeOfChange(std::uint32_t recordIndex,
                                                std::uint64_t changeIndex) const {
    const SignalStore& store = storeAt(recordIndex);
    ENZAN_ASSERT(changeIndex < store.times.size(), "no such value change");
    return store.times[static_cast<std::size_t>(changeIndex)];
}

eir::BitVector WaveformDatabase::valueOfChange(std::uint32_t recordIndex,
                                               std::uint64_t changeIndex) const {
    const SignalStore& store = storeAt(recordIndex);
    ENZAN_ASSERT(changeIndex < store.times.size(), "no such value change");

    const std::span<const eir::Word> words = valueWordsOf(store, changeIndex, recordIndex);
    return eir::BitVector::from(eir::ConstBitRef(words.data(), store.info.width));
}

std::optional<eir::BitVector> WaveformDatabase::valueAt(std::uint32_t recordIndex,
                                                        runtime::SimTime time) const {
    const SignalStore& store = storeAt(recordIndex);

    const auto after = std::ranges::upper_bound(store.times, time);
    if (after == store.times.begin()) return std::nullopt;

    const auto changeIndex = static_cast<std::uint64_t>(std::prev(after) - store.times.begin());
    return valueOfChange(recordIndex, changeIndex);
}

void WaveformDatabase::changesIn(std::uint32_t recordIndex, runtime::SimTime from,
                                 runtime::SimTime to, std::vector<Change>& out) const {
    const SignalStore& store = storeAt(recordIndex);
    if (from >= to) return;

    const auto first = std::ranges::lower_bound(store.times, from);
    const auto last = std::lower_bound(first, store.times.end(), to);

    for (auto position = first; position != last; ++position) {
        const auto changeIndex = static_cast<std::uint64_t>(position - store.times.begin());
        out.push_back(Change{.time = *position, .value = valueOfChange(recordIndex, changeIndex)});
    }
}

void WaveformDatabase::sample(std::uint32_t recordIndex, runtime::SimTime from, runtime::SimTime to,
                              std::span<Sample> out) const {
    ENZAN_ASSERT(from < to, "a sampled range must cover some time");
    ENZAN_ASSERT(!out.empty(), "sampling needs somewhere to put the result");
    ENZAN_ASSERT(out.size() <= MaxSampleColumns, "far more columns than any display has");

    const SignalStore& store = storeAt(recordIndex);

    // Columns partition [from, to) evenly, with the rounding remainder spread across them so
    // that the last column ends exactly at `to`. Accumulating a truncated width instead would
    // leave the right-hand edge of a zoomed-out view short by up to one column.
    const std::uint64_t columns = out.size();
    const runtime::SimTime span = (to - from) / columns;
    const runtime::SimTime remainder = (to - from) % columns;
    const auto edgeAt = [&](std::uint64_t column) {
        return from + (span * column) + ((remainder * column) / columns);
    };

    // The columns are scanned in increasing time, so each binary search starts where the last
    // one ended. That is what keeps the whole pass logarithmic in the trace length rather than
    // linear in the changes it covers (section 10.3).
    auto searchFrom = store.times.begin();

    for (std::size_t column = 0; column < out.size(); ++column) {
        const runtime::SimTime start = edgeAt(column);
        const runtime::SimTime end = column + 1 == out.size() ? to : edgeAt(column + 1);

        const auto after = std::upper_bound(searchFrom, store.times.end(), start);
        const auto beforeEnd = std::lower_bound(after, store.times.end(), end);

        const auto low = static_cast<std::uint64_t>(after - store.times.begin());
        const auto high = static_cast<std::uint64_t>(beforeEnd - store.times.begin());

        // The change in effect at the left edge is the one before the first change after it,
        // and it is part of what the column shows, so the unknown test spans it too.
        const std::uint64_t base = low > 0 ? low - 1 : 0;

        out[column] = Sample{
            .startTime = start,
            .transitions = high - low,
            .changeIndex = low > 0 ? low - 1 : Sample::NoChange,
            .anyUnknown = store.unknownPrefix[static_cast<std::size_t>(high)]
                          > store.unknownPrefix[static_cast<std::size_t>(base)],
        };

        searchFrom = beforeEnd;
    }
}

std::uint64_t WaveformDatabase::indexBytes() const noexcept {
    std::uint64_t total = 0;
    for (const SignalStore& store : signalList) {
        total += store.times.size() * sizeof(runtime::SimTime);
        total += store.unknownPrefix.size() * sizeof(std::uint32_t);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Residency
// ---------------------------------------------------------------------------

std::span<const eir::Word> WaveformDatabase::valueWordsOf(const SignalStore& store,
                                                          std::uint64_t changeIndex,
                                                          std::uint32_t signalRecordIndex) const {
    const auto chunkIndex = static_cast<std::uint32_t>(changeIndex / options.chunkCapacity);
    const std::size_t offset = (changeIndex % options.chunkCapacity) * store.valueWords;

    const Chunk& chunk = store.chunks[chunkIndex];
    if (!chunk.resident) {
        reload(signalRecordIndex, chunkIndex);
    } else {
        touch(signalRecordIndex, chunkIndex);
    }

    return std::span<const eir::Word>(chunk.values).subspan(offset, store.valueWords);
}

void WaveformDatabase::touch(std::uint32_t recordIndex, std::uint32_t chunkIndex) const {
    const Chunk& chunk = signalList[recordIndex].chunks[chunkIndex];
    if (!chunk.evictable) return;
    residentChunks.splice(residentChunks.begin(), residentChunks, chunk.lruPosition);
}

void WaveformDatabase::reload(std::uint32_t recordIndex, std::uint32_t chunkIndex) const {
    const SignalStore& store = signalList[recordIndex];
    const Chunk& chunk = store.chunks[chunkIndex];

    ENZAN_ASSERT(chunk.fileOffset != Chunk::NotSpilled, "a chunk was freed without being written");

    const std::uint64_t bytes = chunkBytes(store);

    // Room is made before the read rather than after it, so that enforcing the budget cannot
    // evict the chunk this call is in the middle of bringing back.
    while (residentBytes + bytes > options.memoryBudgetBytes && !residentChunks.empty()) {
        const auto victim = std::prev(residentChunks.end());
        spill(victim->first, victim->second);
    }

    chunk.values.resize(bytes / BytesPerWord);

    const ssize_t read =
        ::pread(scratchFd, chunk.values.data(), bytes, static_cast<off_t>(chunk.fileOffset));
    ENZAN_ASSERT(read >= 0 && std::cmp_equal(read, bytes),
                 "the waveform scratch file could not be read back");
    (void)read;

    chunk.resident = true;
    residentBytes += bytes;
    residentChunks.emplace_front(recordIndex, chunkIndex);
    chunk.lruPosition = residentChunks.begin();
    chunk.evictable = true;
}

void WaveformDatabase::enforceBudget() const {
    while (residentBytes > options.memoryBudgetBytes && !residentChunks.empty()) {
        const auto victim = std::prev(residentChunks.end());
        spill(victim->first, victim->second);
    }
}

void WaveformDatabase::spill(std::uint32_t recordIndex, std::uint32_t chunkIndex) const {
    const SignalStore& store = signalList[recordIndex];
    const Chunk& chunk = store.chunks[chunkIndex];

    residentChunks.erase(chunk.lruPosition);
    chunk.evictable = false;

    if (!openScratchFile()) {
        // Without a scratch file the only alternative to holding the data is losing it, and
        // losing a waveform silently is worse than exceeding a budget. The chunk stays
        // resident and simply leaves the eviction order, so it is not reconsidered.
        return;
    }

    const std::uint64_t bytes = chunkBytes(store);

    // A sealed chunk's values never change, so a chunk that has been written once keeps its
    // region for the life of the database and re-evicting it costs no I/O at all.
    if (chunk.fileOffset == Chunk::NotSpilled) {
        chunk.fileOffset = scratchSize;
        scratchSize += bytes;
        spilledBytes += bytes;

        const ssize_t written =
            ::pwrite(scratchFd, chunk.values.data(), bytes, static_cast<off_t>(chunk.fileOffset));
        if (written < 0 || std::cmp_not_equal(written, bytes)) {
            // The write failed — a full filesystem is the usual reason. Keep the chunk in
            // memory and forget the region, rather than freeing data that cannot be read back.
            chunk.fileOffset = Chunk::NotSpilled;
            spilledBytes -= bytes;
            return;
        }
    }

    chunk.values.clear();
    chunk.values.shrink_to_fit();
    chunk.resident = false;
    residentBytes -= bytes;
}

bool WaveformDatabase::openScratchFile() const {
    if (scratchFd >= 0) return true;
    if (scratchTried) return false;
    scratchTried = true;

    std::filesystem::path directory = options.spillDirectory;
    if (directory.empty()) {
        std::error_code ignored;
        directory = std::filesystem::temp_directory_path(ignored);
        if (directory.empty()) return false;
    }

    std::string pattern = (directory / "enzan-wave-XXXXXX").string();
    scratchFd = ::mkstemp(pattern.data());
    if (scratchFd < 0) return false;

    // Unlinked immediately: the file is scratch space with no value outside this process, and
    // unlinking now means it cannot survive a crash and fill the user's temporary directory.
    ::unlink(pattern.c_str());
    return true;
}

} // namespace enzan::wavedb
