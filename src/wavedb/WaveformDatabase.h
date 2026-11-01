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
    /// A configurable budget with spill to disk rather than unbounded growth. The budget governs
    /// the *values*, which are the bulk; the time index stays resident because it is what makes
    /// rendering pixel-bounded (see the class comment).
    std::uint64_t memoryBudgetBytes = 256ULL << 20U;

    /// Value changes per chunk. Chunks are the unit of spilling.
    ///
    /// Large enough that a spill is a worthwhile amount of I/O, small enough that reloading
    /// one to answer a query does not drag in an unreasonable amount of unrelated data.
    std::uint32_t chunkCapacity = 1024;

    /// Where the scratch file is created. Empty means the system temporary directory.
    std::filesystem::path spillDirectory;
};

/// The absence of a value change, where an index into a signal's changes is expected.
inline constexpr std::uint64_t NoChange = ~std::uint64_t{0};

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
};

/// One recorded value change, as `changesIn` returns it.
struct Change {
    runtime::SimTime time = 0;
    eir::BitVector value;
};

/// What a search for a value asks of one signal: that the bits it names hold the values it gives.
///
/// Built a range of bits at a time, as a row of the wave window shows them, so that the bits of
/// one signal shown by several rows of a group are asked of the signal once. Laid out as a stored
/// value of the signal is, both planes, with a mask picking out the bits asked, so that a stored
/// value is compared with it where it lies.
class BitPattern {
public:
    /// A pattern over a signal `width` bits wide, asking nothing yet.
    explicit BitPattern(std::uint32_t width);

    /// Asks that bits `[lowBit, highBit]` hold `value`, which is as wide as they are. Returns
    /// false, having asked nothing, when one of them is asked already to hold something else: no
    /// value of the signal meets the pattern then.
    bool require(std::uint32_t highBit, std::uint32_t lowBit, eir::ConstBitRef value);

    /// True when the value stored at `stored`, both of its planes, holds every bit asked.
    [[nodiscard]] bool matches(const eir::Word* stored) const noexcept {
        for (std::uint32_t word = firstWord; word <= lastWord; ++word) {
            const eir::Word differs = (stored[word] ^ expected[word])
                                      | (stored[planeWords + word] ^ expected[planeWords + word]);
            if ((differs & mask[word]) != 0) return false;
        }
        return true;
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return bits; }

private:
    std::uint32_t bits;
    std::uint32_t planeWords;

    /// The words holding a bit asked, which are all `matches` reads. Empty, first past last, while
    /// nothing is asked.
    std::uint32_t firstWord = ~std::uint32_t{0};
    std::uint32_t lastWord = 0;

    std::vector<eir::Word> expected;
    std::vector<eir::Word> mask;
};

/// One signal's part in a search for a value.
struct SearchTerm {
    std::uint32_t recordIndex = 0;
    BitPattern pattern;
};

/// What a digit written `X` or `Z` asks of the bits it covers: some of them unknown and not all,
/// or, none of them unknown, some of them high impedance and not all.
///
/// A digit of a row joined from several signals can cover bits of more than one of them, and of
/// a parameter beside them, so it is asked of a search as a whole rather than of one term, and
/// names each bit by the term whose signal holds it.
struct DigitTerm {
    /// `Unknown` for an `X`, `HighZ` for a `Z`.
    eir::Logic kind = eir::Logic::Unknown;

    /// Bits of one word of a plane of a term's signal.
    struct Bits {
        std::uint32_t term = 0;
        std::uint32_t word = 0;
        eir::Word mask = 0;
    };

    std::vector<Bits> bits;

    /// Every bit the digit covers, and of those no signal holds, which are the same at every
    /// time, how many are unknown and how many high impedance.
    std::uint32_t count = 0;
    std::uint32_t fixedUnknown = 0;
    std::uint32_t fixedHighZ = 0;

    /// Covers bit `bit` of the signal of the search's term `term`. A bit covered twice, as a row
    /// showing a signal twice covers it, counts twice.
    void cover(std::uint32_t term, std::uint32_t bit);

    /// Covers a bit no signal holds, which is `value` at every time.
    void coverFixed(eir::Logic value);

    /// True when the digit shows as it asks, with `unknown` of the bits signals hold unknown and
    /// `highZ` of them high impedance.
    [[nodiscard]] bool shows(std::uint32_t unknown, std::uint32_t highZ) const noexcept;
};

/// How far a search for a value got.
struct SearchResult {
    enum class Outcome : std::uint8_t {
        /// `time` is where what was searched for starts to hold.
        Found,

        /// It starts to hold nowhere the search could look.
        Absent,

        /// The search examined as many changes as it was allowed to first. `time` is the last it
        /// examined, and a search from there goes on where this one stopped.
        Unfinished,
    };

    Outcome outcome = Outcome::Absent;
    runtime::SimTime time = runtime::NoTime;

    friend bool operator==(const SearchResult&, const SearchResult&) = default;
};

/// The in-memory waveform database.
///
/// The structure is per-signal append-only chunks of value changes, plus a time index for random
/// access. The split between the two is what the memory budget and pixel-bounded rendering turn on,
/// so it is worth stating plainly.
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
/// That is what makes pixel-bounded rendering achievable (DESIGN_GUIDE.md section 10.3): **the cost
/// of drawing a window is a function of the number of pixel columns, not of the number of value
/// changes in it.** A column spanning a million transitions costs one binary search, the same as a
/// column spanning none. Values are read only for the columns that are constant enough to print
/// one, which bounds reads — and therefore chunk reloads — by the pixel count too.
///
/// **That bound is for a whole signal.** Summarising a *bit range* of one cannot be answered from
/// the index, because the index counts changes of the signal and says nothing about which bits
/// each of them moved; the `sample` overload taking a range reads values instead and costs what
/// the changes in the range cost. It exists because the alternative was a wrong picture rather
/// than a slow one — see that overload for the whole of the argument.
///
/// The database is written by one producer in increasing time order and read concurrently by the
/// view, which shows a run as it progresses. It is not thread-safe: the GUI process holds its own
/// copy, fed over IPC, and the kernel process holds the authoritative one.
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
    /// appended: the caller has already filtered those (the kernel reports changes only),
    /// and second-guessing it here would hide a producer that had not.
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

    /// Index of the first change strictly after `time`, or `NoChange`.
    ///
    /// Edge navigation is a query rather than a scan because it is what a reader does most in
    /// a wave window: snapping a cursor to the edge under the pointer, and stepping a cursor
    /// from edge to edge along a signal. Both happen per keystroke and per click, on traces
    /// with millions of transitions.
    [[nodiscard]] std::uint64_t nextChangeAfter(std::uint32_t recordIndex,
                                                runtime::SimTime time) const;

    /// Index of the last change strictly before `time`, or `NoChange`.
    [[nodiscard]] std::uint64_t previousChangeBefore(std::uint32_t recordIndex,
                                                     runtime::SimTime time) const;

    /// Index of the change whose time is closest to `time`, or `NoChange` when the signal has
    /// none. A tie goes to the earlier change, so that the answer does not depend on which
    /// side of an edge a click landed by a rounding.
    [[nodiscard]] std::uint64_t nearestChange(std::uint32_t recordIndex,
                                              runtime::SimTime time) const;

    /// The first time after `after`, and before `limit`, at which every term starts to hold at
    /// once: each signal holds the bits asked of it, where an instant earlier one of them did not,
    /// or had recorded nothing. One term to a signal; with none, nothing ever starts.
    ///
    /// **A value has no index**, so finding one is a walk over the changes, and this is that walk
    /// done where the values are kept: each compared in place with its term, a chunk at a time,
    /// and the signals of several terms stepped through together, in time order, so that a time at
    /// which some of them change is tested once all of them have. Copying each value out and
    /// comparing it after a binary search for its time cost from a hundred nanoseconds a change to
    /// microseconds for a row joined from many.
    ///
    /// At most about `budget` changes are examined, a few more for every term, since setting a term
    /// up costs about what examining a few changes does. Two changes of one signal share a time
    /// only when a value was written twice in one time step, and the signal held the last of them
    /// at that time, as `valueAt` says, so only the last is examined.
    ///
    /// Nothing is kept: what a search holds is a cursor a term, and a chunk it reads is read
    /// through the memory budget like any other, so a search of a thousand signals stays inside it.
    ///
    /// Each of `digits` must show as well, its bits named by index into `terms`. A digit is tested
    /// only at a time every term holds, which a search of partly unknown digits is the rarer for.
    [[nodiscard]] SearchResult nextTimeHolding(std::span<const SearchTerm> terms,
                                               runtime::SimTime after, runtime::SimTime limit,
                                               std::uint64_t budget,
                                               std::span<const DigitTerm> digits = {}) const;

    /// The last time before `before`, and before `limit`, at which every term starts to hold at
    /// once, searched for as `nextTimeHolding` searches.
    [[nodiscard]] SearchResult previousTimeHolding(std::span<const SearchTerm> terms,
                                                   runtime::SimTime before, runtime::SimTime limit,
                                                   std::uint64_t budget,
                                                   std::span<const DigitTerm> digits = {}) const;

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

    /// Summarises only bits `[lowBit, highBit]` of the signal, as a row showing a slice needs.
    ///
    /// **A slice's transitions are not the signal's**, and summarising the signal for a row that
    /// displays one bit of it is what made an unpacked bus unreadable: every column in which the
    /// *bus* changed reported a transition, so a bit that never moved was drawn as a solid busy
    /// bar for the whole width of the window, and only zooming in until each column held no
    /// change at all resolved it into the flat line it always was. The same applied to
    /// `anyUnknown`: one X bit made every bit of the bus look unknown.
    ///
    /// Exact, and it costs more than the whole-signal overload: answering it needs the values of
    /// the changes in the range, so the work is proportional to the changes the range covers
    /// rather than to the number of columns, and it reads the value payload where the other one
    /// touches only the resident index. That is a deliberate trade — a correct picture comes
    /// before a fast one — and it is bounded in the two ways that matter: the renderer
    /// caches a summary until the range changes, so a scroll or a repaint pays nothing, and only
    /// a row the reader has actually sliced or unpacked takes this path at all.
    ///
    /// A per-bit change index would restore the cheaper bound and is the obvious next move if
    /// this is ever measured to matter; it is not built yet because it costs memory per bit of
    /// every recorded signal, whether or not anyone looks at one.
    void sample(std::uint32_t recordIndex, runtime::SimTime from, runtime::SimTime to,
                std::uint32_t highBit, std::uint32_t lowBit, std::span<Sample> out) const;

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

    /// Returns the values of a whole chunk, loading it if it was spilled. Valid until another
    /// chunk is loaded.
    [[nodiscard]] std::span<const eir::Word> chunkWordsOf(std::uint32_t recordIndex,
                                                          std::uint32_t chunkIndex) const;

    /// A search's place in one signal's changes. Defined with the searches that use it.
    struct SearchCursor;

    /// The value of change `change` of the cursor's signal, both planes, read through the chunk
    /// the cursor last read where no chunk has been evicted since. Valid until another chunk is
    /// loaded.
    [[nodiscard]] const eir::Word* wordsAt(SearchCursor& cursor, std::uint64_t change) const;

    /// True when the change `change` of the cursor's signal holds its term.
    [[nodiscard]] bool holdsAt(SearchCursor& cursor, std::uint64_t change) const;

    /// True when every one of `digits` shows, with each cursor standing on a change.
    [[nodiscard]] bool digitsShow(std::vector<SearchCursor>& cursors,
                                  std::span<const DigitTerm> digits) const;

    /// The searches of one term, a tight walk over one signal's changes.
    [[nodiscard]] SearchResult nextTimeHoldingOne(const SearchTerm& term, runtime::SimTime after,
                                                  runtime::SimTime limit,
                                                  std::uint64_t budget) const;
    [[nodiscard]] SearchResult previousTimeHoldingOne(const SearchTerm& term,
                                                      runtime::SimTime before,
                                                      runtime::SimTime limit,
                                                      std::uint64_t budget) const;

    /// The first or last change in `[from, to)` at which the term's signal starts to hold it.
    [[nodiscard]] std::uint64_t firstStartIn(const SearchTerm& term, std::uint64_t from,
                                             std::uint64_t to) const;
    [[nodiscard]] std::uint64_t lastStartIn(const SearchTerm& term, std::uint64_t from,
                                            std::uint64_t to) const;

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
    /// contents are observable through the GUI, and observable ordering must be reproducible.
    std::map<std::uint32_t, std::uint32_t> indexBySignal;

    runtime::SimTime latest = 0;

    // The eviction bookkeeping is mutable because a read may have to bring a chunk back, and
    // reloading is a cache operation: it changes what is in memory, never what the database
    // holds. Every query below is logically const.

    /// Resident sealed chunks as (database index, chunk index), most recently used first.
    mutable std::list<std::pair<std::uint32_t, std::uint32_t>> residentChunks;

    mutable std::uint64_t residentBytes = 0;
    mutable std::uint64_t spilledBytes = 0;

    /// Counts the chunks whose values have been freed, so that a search holding a chunk's words
    /// can tell that they may have gone.
    mutable std::uint64_t evictions = 0;

    /// Scratch file descriptor, or -1. The file is unlinked as soon as it is created, so it
    /// cannot outlive the process even if the process dies without unwinding.
    mutable int scratchFd = -1;

    /// True once creating the scratch file has been tried, successfully or not.
    mutable bool scratchTried = false;

    mutable std::uint64_t scratchSize = 0;
};

} // namespace enzan::wavedb
