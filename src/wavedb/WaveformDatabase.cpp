// SPDX-License-Identifier: GPL-3.0-or-later

#include "wavedb/WaveformDatabase.h"

#include "common/Assert.h"
#include "eir/BitOps.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <queue>
#include <string>
#include <utility>
#include <vector>

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
///
/// Marked maybe_unused because its only use is an assertion, and a Release build compiles
/// those away — which leaves the name with no reader and earns clang's
/// `-Wunused-const-variable`. The alternative is to write the number into the assertion, and a
/// bound worth eight lines of explanation is worth a name.
[[maybe_unused]] constexpr std::size_t MaxSampleColumns = 1U << 20U;

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

std::uint64_t WaveformDatabase::nextChangeAfter(std::uint32_t recordIndex,
                                                runtime::SimTime time) const {
    const SignalStore& store = storeAt(recordIndex);

    const auto after = std::ranges::upper_bound(store.times, time);
    if (after == store.times.end()) return NoChange;
    return static_cast<std::uint64_t>(after - store.times.begin());
}

std::uint64_t WaveformDatabase::previousChangeBefore(std::uint32_t recordIndex,
                                                     runtime::SimTime time) const {
    const SignalStore& store = storeAt(recordIndex);

    const auto notBefore = std::ranges::lower_bound(store.times, time);
    if (notBefore == store.times.begin()) return NoChange;
    return static_cast<std::uint64_t>(notBefore - store.times.begin()) - 1;
}

std::uint64_t WaveformDatabase::nearestChange(std::uint32_t recordIndex,
                                              runtime::SimTime time) const {
    const SignalStore& store = storeAt(recordIndex);
    if (store.times.empty()) return NoChange;

    const std::uint64_t later = nextChangeAfter(recordIndex, time);
    const std::uint64_t earlier = previousChangeBefore(recordIndex, time);

    // A change exactly at `time` is neither strictly before nor strictly after it, so it is
    // looked for separately rather than being missed by both.
    const auto atTime = std::ranges::lower_bound(store.times, time);
    if (atTime != store.times.end() && *atTime == time) {
        return static_cast<std::uint64_t>(atTime - store.times.begin());
    }

    if (later == NoChange) return earlier;
    if (earlier == NoChange) return later;

    const runtime::SimTime forward = store.times[static_cast<std::size_t>(later)] - time;
    const runtime::SimTime backward = time - store.times[static_cast<std::size_t>(earlier)];
    return backward <= forward ? earlier : later;
}

// ---------------------------------------------------------------------------
// Searching for a value
// ---------------------------------------------------------------------------

BitPattern::BitPattern(std::uint32_t width)
    : bits(width),
      planeWords(eir::wordsFor(width)),
      expected(eir::storageWordsFor(width)),
      mask(planeWords) {}

bool BitPattern::require(std::uint32_t highBit, std::uint32_t lowBit, eir::ConstBitRef value) {
    ENZAN_ASSERT(lowBit <= highBit && highBit < bits, "a bit range must lie inside the signal");
    ENZAN_ASSERT(value.width() == highBit - lowBit + 1, "a value must be as wide as its bits");

    eir::BitVector placed = eir::BitVector::filled(bits, eir::Logic::Zero);
    eir::bitops::assignPartAt(placed.mutableRef(), value, lowBit);
    const eir::Word* words = placed.ref().aval();

    std::vector<eir::Word> asked(planeWords);
    for (std::uint32_t bit = lowBit; bit <= highBit; ++bit)
        asked[bit / eir::BitsPerWord] |= eir::Word{1} << (bit % eir::BitsPerWord);

    // Checked whole before anything is changed, so that a refusal leaves the pattern as it was.
    for (std::uint32_t word = 0; word < planeWords; ++word) {
        const eir::Word differs = (expected[word] ^ words[word])
                                  | (expected[planeWords + word] ^ words[planeWords + word]);
        if ((differs & mask[word] & asked[word]) != 0) return false;
    }

    for (std::uint32_t word = 0; word < planeWords; ++word) {
        expected[word] |= words[word] & asked[word];
        expected[planeWords + word] |= words[planeWords + word] & asked[word];
        mask[word] |= asked[word];
    }
    firstWord = std::min(firstWord, lowBit / eir::BitsPerWord);
    lastWord = std::max(lastWord, highBit / eir::BitsPerWord);
    return true;
}

/// A search's place in one signal's changes, and whether the signal holds its term there.
///
/// It keeps the address of the chunk it last read rather than asking for it at every change,
/// which is most of what reading a value costs, and asks again once the chunk changes or any
/// chunk has been evicted since, so that it never reads values a reload has freed.
struct WaveformDatabase::SearchCursor {
    const SearchTerm* term = nullptr;
    const SignalStore* store = nullptr;

    /// Forwards, the first change not yet examined; backwards, one past the last.
    std::uint64_t next = 0;

    bool holds = false;

    /// The changes of the chunk last read, `[chunkStart, chunkEnd)`, where its values are, and
    /// how many chunks had been evicted when they were read.
    std::uint64_t chunkStart = 0;
    std::uint64_t chunkEnd = 0;
    const eir::Word* words = nullptr;
    std::uint64_t evictionsSeen = 0;
};

bool WaveformDatabase::holdsAt(SearchCursor& cursor, std::uint64_t change) const {
    // Compared with the bounds of the chunk rather than divided into one, since a division costs
    // more than the comparison the values are read for.
    if (change < cursor.chunkStart || change >= cursor.chunkEnd
        || cursor.evictionsSeen != evictions) {
        const auto chunkIndex = static_cast<std::uint32_t>(change / options.chunkCapacity);
        cursor.words = chunkWordsOf(cursor.term->recordIndex, chunkIndex).data();
        cursor.chunkStart = std::uint64_t{chunkIndex} * options.chunkCapacity;
        cursor.chunkEnd = cursor.chunkStart + options.chunkCapacity;
        cursor.evictionsSeen = evictions;
    }
    const std::uint64_t offset = (change - cursor.chunkStart) * cursor.store->valueWords;
    return cursor.term->pattern.matches(cursor.words + offset);
}

SearchResult WaveformDatabase::nextTimeHolding(std::span<const SearchTerm> terms,
                                               runtime::SimTime after, runtime::SimTime limit,
                                               std::uint64_t budget) const {
    if (terms.empty()) return {};
    if (terms.size() == 1) return nextTimeHoldingOne(terms.front(), after, limit, budget);

    std::vector<SearchCursor> cursors(terms.size());
    std::size_t holding = 0;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        SearchCursor& cursor = cursors[index];
        cursor.term = &terms[index];
        cursor.store = &storeAt(terms[index].recordIndex);
        cursor.next = static_cast<std::uint64_t>(
            std::ranges::upper_bound(cursor.store->times, after) - cursor.store->times.begin());
        cursor.holds = cursor.next > 0 && holdsAt(cursor, cursor.next - 1);
        holding += cursor.holds ? 1 : 0;
    }

    // The cursors by the time of their next change, earliest first.
    using Pending = std::pair<runtime::SimTime, std::size_t>;
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> pending;
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        const SearchCursor& cursor = cursors[index];
        if (cursor.next < cursor.store->times.size())
            pending.emplace(cursor.store->times[cursor.next], index);
    }

    const std::uint64_t allowed = std::max(budget, std::uint64_t{8} * terms.size());
    std::uint64_t examined = 0;
    while (!pending.empty()) {
        const runtime::SimTime time = pending.top().first;
        if (time >= limit) return {};
        const bool held = holding == cursors.size();

        // Every signal changing at this time changes before the terms are tested, since they all
        // change at once.
        while (!pending.empty() && pending.top().first == time) {
            SearchCursor& cursor = cursors[pending.top().second];
            const std::size_t index = pending.top().second;
            pending.pop();

            const std::vector<runtime::SimTime>& times = cursor.store->times;
            while (cursor.next < times.size() && times[cursor.next] == time) {
                ++cursor.next;
                ++examined;
            }
            const bool holds = holdsAt(cursor, cursor.next - 1);
            holding = holding - (cursor.holds ? 1 : 0) + (holds ? 1 : 0);
            cursor.holds = holds;
            if (cursor.next < times.size()) pending.emplace(times[cursor.next], index);
        }

        if (holding == cursors.size() && !held)
            return {.outcome = SearchResult::Outcome::Found, .time = time};
        if (examined >= allowed)
            return {.outcome = SearchResult::Outcome::Unfinished, .time = time};
    }
    return {};
}

SearchResult WaveformDatabase::previousTimeHolding(std::span<const SearchTerm> terms,
                                                   runtime::SimTime before, runtime::SimTime limit,
                                                   std::uint64_t budget) const {
    if (terms.empty()) return {};
    if (terms.size() == 1) return previousTimeHoldingOne(terms.front(), before, limit, budget);

    // Nothing at or past the limit is known yet, so the search starts below it.
    const runtime::SimTime bound = std::min(before, limit);

    std::vector<SearchCursor> cursors(terms.size());
    std::size_t holding = 0;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        SearchCursor& cursor = cursors[index];
        cursor.term = &terms[index];
        cursor.store = &storeAt(terms[index].recordIndex);
        cursor.next = static_cast<std::uint64_t>(
            std::ranges::lower_bound(cursor.store->times, bound) - cursor.store->times.begin());
        cursor.holds = cursor.next > 0 && holdsAt(cursor, cursor.next - 1);
        holding += cursor.holds ? 1 : 0;
    }

    // The cursors by the time of the change they stand on, latest first. What the signals hold
    // from the latest of them is what they hold now; stepping each back past it gives what they
    // held an instant before.
    using Pending = std::pair<runtime::SimTime, std::size_t>;
    std::priority_queue<Pending> pending;
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        const SearchCursor& cursor = cursors[index];
        if (cursor.next > 0) pending.emplace(cursor.store->times[cursor.next - 1], index);
    }

    const std::uint64_t allowed = std::max(budget, std::uint64_t{8} * terms.size());
    std::uint64_t examined = 0;
    while (!pending.empty()) {
        const runtime::SimTime time = pending.top().first;
        const bool holds = holding == cursors.size();

        while (!pending.empty() && pending.top().first == time) {
            SearchCursor& cursor = cursors[pending.top().second];
            const std::size_t index = pending.top().second;
            pending.pop();

            const std::vector<runtime::SimTime>& times = cursor.store->times;
            while (cursor.next > 0 && times[cursor.next - 1] == time) {
                --cursor.next;
                ++examined;
            }
            const bool held = cursor.next > 0 && holdsAt(cursor, cursor.next - 1);
            holding = holding - (cursor.holds ? 1 : 0) + (held ? 1 : 0);
            cursor.holds = held;
            if (cursor.next > 0) pending.emplace(times[cursor.next - 1], index);
        }

        if (holds && holding != cursors.size())
            return {.outcome = SearchResult::Outcome::Found, .time = time};
        if (examined >= allowed)
            return {.outcome = SearchResult::Outcome::Unfinished, .time = time};
    }
    return {};
}

SearchResult WaveformDatabase::nextTimeHoldingOne(const SearchTerm& term, runtime::SimTime after,
                                                  runtime::SimTime limit,
                                                  std::uint64_t budget) const {
    const std::vector<runtime::SimTime>& times = storeAt(term.recordIndex).times;
    const auto first =
        static_cast<std::uint64_t>(std::ranges::upper_bound(times, after) - times.begin());
    const auto stop =
        static_cast<std::uint64_t>(std::ranges::lower_bound(times, limit) - times.begin());
    if (first >= stop) return {};

    // As many changes as the budget allows, ending with a whole time, so that a search going on
    // from that time starts where this one stopped.
    std::uint64_t last = std::min(stop, first + std::max(budget, std::uint64_t{1}));
    while (last < stop && times[last] == times[last - 1]) ++last;

    const std::uint64_t found = firstStartIn(term, first, last);
    if (found != NoChange) return {.outcome = SearchResult::Outcome::Found, .time = times[found]};
    if (last == stop) return {};
    return {.outcome = SearchResult::Outcome::Unfinished, .time = times[last - 1]};
}

SearchResult WaveformDatabase::previousTimeHoldingOne(const SearchTerm& term,
                                                      runtime::SimTime before,
                                                      runtime::SimTime limit,
                                                      std::uint64_t budget) const {
    const std::vector<runtime::SimTime>& times = storeAt(term.recordIndex).times;
    const auto end = static_cast<std::uint64_t>(
        std::ranges::lower_bound(times, std::min(before, limit)) - times.begin());
    if (end == 0) return {};

    // As many changes as the budget allows, beginning with a whole time, as forwards.
    const std::uint64_t allowed = std::max(budget, std::uint64_t{1});
    std::uint64_t begin = end > allowed ? end - allowed : 0;
    while (begin > 0 && times[begin] == times[begin - 1]) --begin;

    const std::uint64_t found = lastStartIn(term, begin, end);
    if (found != NoChange) return {.outcome = SearchResult::Outcome::Found, .time = times[found]};
    if (begin == 0) return {};
    return {.outcome = SearchResult::Outcome::Unfinished, .time = times[begin]};
}

std::uint64_t WaveformDatabase::firstStartIn(const SearchTerm& term, std::uint64_t from,
                                             std::uint64_t to) const {
    const SignalStore& store = storeAt(term.recordIndex);
    ENZAN_ASSERT(term.pattern.width() == store.info.width, "a pattern must fit its signal");
    ENZAN_ASSERT(from <= to && to <= store.times.size(), "a range must lie inside the changes");
    if (from == to) return NoChange;

    const std::vector<runtime::SimTime>& times = store.times;
    const auto lastOfItsTime = [&](std::uint64_t change) {
        return change + 1 == times.size() || times[change + 1] != times[change];
    };

    // Whether the signal held the term an instant before the range: at the last change of an
    // earlier time, which is read before the walk, so that the walk holds one chunk at a time.
    std::uint64_t earlier = from;
    while (earlier > 0 && times[earlier - 1] == times[from]) --earlier;
    const BitPattern& pattern = term.pattern;
    bool held =
        earlier > 0 && pattern.matches(valueWordsOf(store, earlier - 1, term.recordIndex).data());

    // A chunk at a time, so that a change costs its comparison and nothing more.
    for (std::uint64_t change = from; change < to;) {
        const auto chunkIndex = static_cast<std::uint32_t>(change / options.chunkCapacity);
        const eir::Word* words = chunkWordsOf(term.recordIndex, chunkIndex).data();
        const std::uint64_t chunkStart = std::uint64_t{chunkIndex} * options.chunkCapacity;
        const std::uint64_t end = std::min(to, chunkStart + options.chunkCapacity);

        for (; change < end; ++change) {
            if (!lastOfItsTime(change)) continue;
            const bool holds = pattern.matches(words + ((change - chunkStart) * store.valueWords));
            if (holds && !held) return change;
            held = holds;
        }
    }
    return NoChange;
}

std::uint64_t WaveformDatabase::lastStartIn(const SearchTerm& term, std::uint64_t from,
                                            std::uint64_t to) const {
    const SignalStore& store = storeAt(term.recordIndex);
    ENZAN_ASSERT(term.pattern.width() == store.info.width, "a pattern must fit its signal");
    ENZAN_ASSERT(from <= to && to <= store.times.size(), "a range must lie inside the changes");
    if (from == to) return NoChange;

    const std::vector<runtime::SimTime>& times = store.times;
    const auto lastOfItsTime = [&](std::uint64_t change) {
        return change + 1 == times.size() || times[change + 1] != times[change];
    };

    // Walking back, a change holding the term is a start when the time before it did not hold
    // it, which is the next change the walk examines. The last one it examines has the time before
    // it outside the range, and is settled after the walk.
    const BitPattern& pattern = term.pattern;
    std::uint64_t candidate = NoChange;
    for (std::uint64_t next = to; next > from;) {
        const auto chunkIndex = static_cast<std::uint32_t>((next - 1) / options.chunkCapacity);
        const eir::Word* words = chunkWordsOf(term.recordIndex, chunkIndex).data();
        const std::uint64_t chunkStart = std::uint64_t{chunkIndex} * options.chunkCapacity;
        const std::uint64_t begin = std::max(from, chunkStart);

        for (; next > begin; --next) {
            const std::uint64_t change = next - 1;
            if (!lastOfItsTime(change)) continue;
            const bool holds = pattern.matches(words + ((change - chunkStart) * store.valueWords));
            if (candidate != NoChange && !holds) return candidate;
            candidate = holds ? change : NoChange;
        }
    }
    if (candidate == NoChange) return NoChange;

    std::uint64_t first = candidate;
    while (first > 0 && times[first - 1] == times[candidate]) --first;
    const bool heldBefore =
        first > 0 && pattern.matches(valueWordsOf(store, first - 1, term.recordIndex).data());
    return heldBefore ? NoChange : candidate;
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
                              std::uint32_t highBit, std::uint32_t lowBit,
                              std::span<Sample> out) const {
    const SignalStore& store = storeAt(recordIndex);
    ENZAN_ASSERT(lowBit <= highBit, "a bit range runs from low to high");
    ENZAN_ASSERT(highBit < store.info.width, "a bit range must lie inside the signal");

    // The whole signal is the cheap case and it is asked for constantly — every row that has not
    // been sliced. Recognising it here rather than at each call site keeps the callers from
    // having to know which overload is the expensive one.
    if (lowBit == 0 && highBit + 1 == store.info.width) {
        sample(recordIndex, from, to, out);
        return;
    }

    sample(recordIndex, from, to, out);

    // What the cheap pass got right: the column boundaries, and which change is in effect at
    // each. What it could not know: whether any of the changes it counted moved *these* bits.
    // So each column's count and unknown flag are recomputed from the values, and everything
    // else it worked out is kept.
    const auto sliceEquals = [&](std::uint64_t left, std::uint64_t right) {
        const eir::ConstBitRef a(valueWordsOf(store, left, recordIndex).data(), store.info.width);
        const eir::ConstBitRef b(valueWordsOf(store, right, recordIndex).data(), store.info.width);
        for (std::uint32_t bit = lowBit; bit <= highBit; ++bit) {
            if (a.get(bit) != b.get(bit)) return false;
        }
        return true;
    };

    const auto sliceUnknown = [&](std::uint64_t change) {
        const eir::ConstBitRef value(valueWordsOf(store, change, recordIndex).data(),
                                     store.info.width);
        for (std::uint32_t bit = lowBit; bit <= highBit; ++bit) {
            const eir::Logic logic = value.get(bit);
            if (logic == eir::Logic::Unknown || logic == eir::Logic::HighZ) return true;
        }
        return false;
    };

    for (Sample& column : out) {
        // The changes the cheap pass counted are the ones strictly inside the column, which sit
        // immediately after the change in effect at its left edge. Recovered from the sample
        // rather than searched for again, since that pass has just worked it out.
        const std::uint64_t counted = column.transitions;
        const std::uint64_t low = column.changeIndex == NoChange ? 0 : column.changeIndex + 1;
        const std::uint64_t high = low + counted;

        column.transitions = 0;

        // The change in effect at the left edge is part of what the column shows, so it counts
        // towards the unknown flag — as it does in the whole-signal pass.
        column.anyUnknown = column.changeIndex != NoChange && sliceUnknown(column.changeIndex);

        for (std::uint64_t change = low; change < high; ++change) {
            // The very first change of a record has no predecessor to differ from: it is the
            // value appearing, which is a transition of everything it covers.
            if (change == 0 || !sliceEquals(change - 1, change)) ++column.transitions;
            column.anyUnknown = column.anyUnknown || sliceUnknown(change);
        }
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
    // linear in the changes it covers.
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
            .changeIndex = low > 0 ? low - 1 : NoChange,
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
    return chunkWordsOf(signalRecordIndex, chunkIndex).subspan(offset, store.valueWords);
}

std::span<const eir::Word> WaveformDatabase::chunkWordsOf(std::uint32_t recordIndex,
                                                          std::uint32_t chunkIndex) const {
    const Chunk& chunk = signalList[recordIndex].chunks[chunkIndex];
    if (!chunk.resident) {
        reload(recordIndex, chunkIndex);
    } else {
        touch(recordIndex, chunkIndex);
    }
    return chunk.values;
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
    ++evictions;
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
