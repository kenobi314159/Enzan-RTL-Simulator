// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Time.h"

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::wavedb {

/// One signal the database holds a trace for.
struct RecordedSignal {
    /// The kernel's index for the signal, as `runtime::Image::signals()` numbers it.
    ///
    /// Kept alongside the database's own dense index because the two differ whenever a scope
    /// filter is in force, and because the kernel's index is what crosses the IPC boundary.
    std::uint32_t signalIndex = 0;

    std::string path;
    std::uint32_t width = 1;
};

/// Tunable database behaviour.
struct DatabaseOptions {
    /// Ceiling on the resident value payload, in bytes.
    ///
    /// Section 7.1 requires a configurable budget with spill to disk rather than unbounded
    /// growth. The budget governs the *values*, which are the bulk; the time index stays
    /// resident because it is what makes rendering pixel-bounded (see the class comment).
    std::uint64_t memoryBudgetBytes = 256ULL << 20U;

    /// Value changes per chunk. Chunks are the unit of spilling.
    ///
    /// Large enough that a spill is a worthwhile amount of I/O, small enough that reloading
    /// one to answer a query does not drag in an unreasonable amount of unrelated data.
    std::uint32_t chunkCapacity = 1024;

    /// Where the scratch file is created. Empty means the system temporary directory.
    std::filesystem::path spillDirectory;
};

/// What the waveform view needs to draw one pixel column.
///
/// Deliberately not a value: `sample` fills an array of these without allocating, and the
/// renderer fetches the value only for the columns where it actually draws one.
struct Sample {
    /// The first instant this column covers.
    runtime::SimTime startTime = 0;

    /// Value changes strictly inside the column, that is at a time in `(startTime, endTime)`.
    ///
    /// Zero means the trace is constant across the column and can be drawn as a level with its
    /// value written on it; anything else means the column is busy and is drawn as a band.
    std::uint64_t transitions = 0;

    /// Index of the change in effect at `startTime`, or `NoChange` before the first one.
    std::uint64_t changeIndex = NoChange;

    /// True when the value in effect anywhere in the column has an X or Z bit.
    bool anyUnknown = false;

    static constexpr std::uint64_t NoChange = ~std::uint64_t{0};
};

/// One recorded value change, as `changesIn` returns it.
struct Change {
    runtime::SimTime time = 0;
    eir::BitVector value;
};

/// The in-memory waveform database (DESIGN_GUIDE.md section 7).
///
/// Section 7.2 fixes the structure: per-signal append-only chunks of value changes, plus a time
/// index for random access. The split between the two is what the requirements of 7.1 and 10.3
/// turn on, so it is worth stating plainly.
///
/// **The time index is resident; the values spill.** Per signal the database keeps an array of
/// change times and a prefix count of changes to an unknown value, both always in memory, and
/// the value payload in fixed-size chunks that are written to a scratch file and freed when the
/// memory budget is exceeded. The index costs twelve bytes per change and the payload sixteen or
/// more, so the budget governs the part that actually grows; and, more importantly, every
/// question the renderer asks — how many transitions fall in this pixel column, was anything
/// unknown in it, which change is in effect at its left edge — is answered from the resident
/// index by binary search and subtraction, touching no values at all.
///
/// That is what makes section 10.3's requirement achievable: **the cost of drawing a window is a
/// function of the number of pixel columns, not of the number of value changes in it.** A column
/// spanning a million transitions costs one binary search, the same as a column spanning none.
/// Values are read only for the columns that are constant enough to print one, which bounds
/// reads — and therefore chunk reloads — by the pixel count too.
///
/// The database is written by one producer in increasing time order and read concurrently by
/// the view (section 7.1 requires live streaming). It is not thread-safe: the GUI process holds
/// its own copy, fed over IPC, and the kernel process holds the authoritative one.
class WaveformDatabase {
public:
    explicit WaveformDatabase(DatabaseOptions databaseOptions = {});

    WaveformDatabase(const WaveformDatabase&) = delete;
    WaveformDatabase& operator=(const WaveformDatabase&) = delete;
    WaveformDatabase(WaveformDatabase&&) = delete;
    WaveformDatabase& operator=(WaveformDatabase&&) = delete;

    ~WaveformDatabase();

    /// Declares a signal and returns its dense database index.
    ///
    /// Declaring the same `signalIndex` twice returns the existing index and changes nothing,
    /// so that a client replaying a hierarchy is idempotent.
    std::uint32_t addSignal(std::uint32_t signalIndex, std::string path, std::uint32_t width);

    [[nodiscard]] std::size_t signalCount() const noexcept { return signalList.size(); }

    [[nodiscard]] const RecordedSignal& signal(std::uint32_t recordIndex) const;

    /// Returns the database index of a kernel signal index, or `NotRecorded`.
    [[nodiscard]] std::uint32_t recordIndexOf(std::uint32_t signalIndex) const;

    /// Appends a value change.
    ///
    /// `time` must not precede the signal's last recorded change, and `value` must be the
    /// signal's declared width. A change to the value the signal already holds is still
    /// appended: the caller has already filtered those (the kernel reports changes only, see
    /// section 6.2), and second-guessing it here would hide a producer that had not.
    void append(std::uint32_t recordIndex, runtime::SimTime time, eir::ConstBitRef value);

    /// Records that the trace is known to be valid up to `time`, whether or not anything
    /// changed there. This is what the time axis is drawn from; it never moves backwards.
    void setLatestTime(runtime::SimTime time) noexcept;

    [[nodiscard]] runtime::SimTime latestTime() const noexcept { return latest; }

    /// Number of changes recorded for a signal.
    [[nodiscard]] std::uint64_t changeCount(std::uint32_t recordIndex) const;

    [[nodiscard]] runtime::SimTime timeOfChange(std::uint32_t recordIndex,
                                                std::uint64_t changeIndex) const;

    /// Returns the value a change set, copied out because the chunk holding it may be
    /// evicted before the caller looks at it again.
    [[nodiscard]] eir::BitVector valueOfChange(std::uint32_t recordIndex,
                                               std::uint64_t changeIndex) const;

    /// The value in effect at `time`, or nothing when `time` precedes the first change.
    [[nodiscard]] std::optional<eir::BitVector> valueAt(std::uint32_t recordIndex,
                                                        runtime::SimTime time) const;

    /// Appends every change with a time in `[from, to)` to `out`.
    ///
    /// For the zoomed-in case, where the caller wants the transitions themselves rather than a
    /// per-column summary. Cost is proportional to the number of changes returned, which is why
    /// the renderer uses `sample` instead whenever a column could span more than a few.
    void changesIn(std::uint32_t recordIndex, runtime::SimTime from, runtime::SimTime to,
                   std::vector<Change>& out) const;

    /// Summarises `[from, to)` into one `Sample` per element of `out`.
    ///
    /// The columns partition the range evenly; rounding is distributed so that the last one
    /// ends exactly at `to` rather than accumulating drift. `from` must be less than `to` and
    /// `out` must not be empty.
    void sample(std::uint32_t recordIndex, runtime::SimTime from, runtime::SimTime to,
                std::span<Sample> out) const;

    // --- memory accounting -------------------------------------------------

    /// Bytes of value payload currently in memory.
    [[nodiscard]] std::uint64_t residentValueBytes() const noexcept { return residentBytes; }

    /// Bytes of value payload written to the scratch file.
    [[nodiscard]] std::uint64_t spilledValueBytes() const noexcept { return spilledBytes; }

    /// Bytes of resident time index, which the budget does not govern.
    [[nodiscard]] std::uint64_t indexBytes() const noexcept;

    static constexpr std::uint32_t NotRecorded = 0xFFFF'FFFFU;

private:
    /// One fixed-size run of values, resident or spilled.
    ///
    /// Only a sealed chunk — one that has reached `chunkCapacity` and will never be appended
    /// to again — may be spilled. The chunk a signal is currently filling stays resident, both
    /// because writing a partial chunk would have to be undone on the next append and because
    /// the live tail is the part a running simulation reads most.
    ///
    /// Every member is `mutable`, because where a chunk's values live is a cache decision and
    /// not part of what the database holds: a query may have to bring one back, and it answers
    /// identically either way. Saying so with `mutable` keeps the query methods honestly const
    /// without a `const_cast`, which would be undefined the moment a caller held the database
    /// through a const reference.
    struct Chunk {
        mutable std::vector<eir::Word> values;

        /// Byte offset in the scratch file, or `NotSpilled` when it has never been written.
        mutable std::uint64_t fileOffset = NotSpilled;

        /// Position in `residentChunks`, valid only while `evictable` is true.
        mutable std::list<std::pair<std::uint32_t, std::uint32_t>>::iterator lruPosition;

        mutable bool resident = true;

        /// True while the chunk sits in the eviction order.
        ///
        /// Distinct from `resident`, because a chunk that could not be written to the scratch
        /// file stays in memory but leaves the order: losing a waveform silently is worse than
        /// exceeding a budget, so it is kept and simply never reconsidered.
        mutable bool evictable = false;

        static constexpr std::uint64_t NotSpilled = ~std::uint64_t{0};
    };

    struct SignalStore {
        RecordedSignal info;

        /// Words of storage one value occupies, both bit planes together.
        std::uint32_t valueWords = 0;

        /// Time of each change, non-decreasing. Resident; see the class comment.
        std::vector<runtime::SimTime> times;

        /// `unknownPrefix[i]` counts the changes before `i` whose value had an X or Z bit.
        /// One longer than `times`, so that a half-open range is a subtraction.
        std::vector<std::uint32_t> unknownPrefix{0};

        std::vector<Chunk> chunks;
    };

    [[nodiscard]] SignalStore& storeAt(std::uint32_t recordIndex);
    [[nodiscard]] const SignalStore& storeAt(std::uint32_t recordIndex) const;

    /// Returns the values of one change, loading its chunk if it was spilled.
    [[nodiscard]] std::span<const eir::Word> valueWordsOf(const SignalStore& store,
                                                          std::uint64_t changeIndex,
                                                          std::uint32_t signalRecordIndex) const;

    /// Makes a chunk's payload resident, reading it back from the scratch file.
    void reload(std::uint32_t recordIndex, std::uint32_t chunkIndex) const;

    /// Moves a resident chunk to the front of the eviction order.
    void touch(std::uint32_t recordIndex, std::uint32_t chunkIndex) const;

    /// Spills least-recently-used sealed chunks until the budget is met, or until nothing
    /// evictable is left.
    void enforceBudget() const;

    void spill(std::uint32_t recordIndex, std::uint32_t chunkIndex) const;

    /// Opens the scratch file on first use. Returns false when it could not be created, in
    /// which case the database simply keeps everything in memory.
    [[nodiscard]] bool openScratchFile() const;

    [[nodiscard]] std::uint64_t chunkBytes(const SignalStore& store) const noexcept;

    DatabaseOptions options;

    std::vector<SignalStore> signalList;

    /// Kernel signal index to database index. Ordered rather than hash-based: the database's
    /// contents are observable through the GUI and section 3.8 makes observable ordering
    /// reproducible.
    std::map<std::uint32_t, std::uint32_t> indexBySignal;

    runtime::SimTime latest = 0;

    // The eviction bookkeeping is mutable because a read may have to bring a chunk back, and
    // reloading is a cache operation: it changes what is in memory, never what the database
    // holds. Every query below is logically const.

    /// Resident sealed chunks as (database index, chunk index), most recently used first.
    mutable std::list<std::pair<std::uint32_t, std::uint32_t>> residentChunks;

    mutable std::uint64_t residentBytes = 0;
    mutable std::uint64_t spilledBytes = 0;

    /// Scratch file descriptor, or -1. The file is unlinked as soon as it is created, so it
    /// cannot outlive the process even if the process dies without unwinding.
    mutable int scratchFd = -1;

    /// True once creating the scratch file has been tried, successfully or not.
    mutable bool scratchTried = false;

    mutable std::uint64_t scratchSize = 0;
};

} // namespace enzan::wavedb
