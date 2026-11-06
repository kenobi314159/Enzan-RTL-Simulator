// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "runtime/Arena.h"
#include "runtime/Format.h"
#include "runtime/Image.h"
#include "runtime/Scheduler.h"
#include "runtime/Time.h"

#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::runtime {

/// Why `Kernel::run` returned.
enum class RunStatus : std::uint8_t {
    /// Still runnable; `run` stopped because it reached the time limit it was given.
    TimeLimitReached,
    /// Nothing is scheduled and no future event exists. The design has run out of work,
    /// which for a testbench without `$finish` is the normal end.
    Quiescent,
    /// `$finish` or `$fatal` executed.
    Finished,
    /// The run was abandoned: the delta limit was exceeded, or time would overflow.
    Error,
    /// A registered `RunInterrupt` asked the run to stop. The design is untouched and
    /// another `run` continues exactly where this one left off.
    Interrupted,
};

std::string_view toString(RunStatus status) noexcept;

/// Asked between time slots whether the current run should stop.
///
/// The GUI must be able to stop a running kernel, and a stalled or slow GUI must never stall the
/// kernel. Both fall out of polling rather than being signalled: the kernel asks when it is between
/// time slots and therefore in a state it can be resumed from, and nothing outside the kernel ever
/// touches its state.
///
/// The poll is deliberately at a time-slot boundary and not finer. All the state a run holds lives
/// in the scheduler and the arena, so stopping there and calling `run` again produces exactly the
/// results an uninterrupted run would have produced, which would not be true of an interruption
/// part-way through a region.
class RunInterrupt {
public:
    RunInterrupt() = default;
    RunInterrupt(const RunInterrupt&) = delete;
    RunInterrupt& operator=(const RunInterrupt&) = delete;
    RunInterrupt(RunInterrupt&&) = delete;
    RunInterrupt& operator=(RunInterrupt&&) = delete;
    virtual ~RunInterrupt() = default;

    /// Called between time slots. Returning true makes `run` return `Interrupted`.
    ///
    /// Must not report diagnostics, touch the arena or schedule anything: it runs in the
    /// middle of a simulation and its only job is to answer the question.
    virtual bool shouldInterrupt() = 0;
};

/// Receives value changes and time advances, for the waveform database.
///
/// An interface here rather than a direct dependency, so that `runtime` does not depend
/// on `wavedb`: the kernel must be usable with no waveform recording at all, and the
/// GUI attaches and detaches its own listeners.
class ValueChangeListener {
public:
    ValueChangeListener() = default;
    ValueChangeListener(const ValueChangeListener&) = delete;
    ValueChangeListener& operator=(const ValueChangeListener&) = delete;
    ValueChangeListener(ValueChangeListener&&) = delete;
    ValueChangeListener& operator=(ValueChangeListener&&) = delete;
    virtual ~ValueChangeListener() = default;

    /// Called after a signal's new value is in the arena. Only called for changes, never
    /// for a write of the value a signal already held.
    virtual void onValueChange(std::uint32_t signalIndex, eir::ConstBitRef value) = 0;

    /// Called when simulated time advances, before anything at the new time runs.
    virtual void onTimeAdvance(SimTime time) = 0;

    /// `$dumpfile`. The default ignores it, because the file is normally chosen on the
    /// command line and a design that names one should not override that silently.
    virtual void onDumpFile([[maybe_unused]] std::string_view path) {}

    /// `$dumpvars`.
    virtual void onDumpVars() {}
};

/// The points in the region ordering at which a PLI application runs.
///
/// The PLI regions are hook points rather than queues, and DESIGN_GUIDE.md section 9.3 says where
/// each callback fires; this is the kernel's half of that table. An interface rather than a
/// dependency for the reason `ValueChangeListener` is one: `runtime` must not depend on `pli`, and
/// a kernel with no application loaded pays one null test per hook and nothing else.
///
/// Every hook may re-enter the kernel — write a signal, schedule a non-blocking update, ask
/// for the run to finish — because that is what an application's callbacks do.
class PliHooks {
public:
    PliHooks() = default;
    PliHooks(const PliHooks&) = delete;
    PliHooks& operator=(const PliHooks&) = delete;
    PliHooks(PliHooks&&) = delete;
    PliHooks& operator=(PliHooks&&) = delete;
    virtual ~PliHooks() = default;

    /// Called once a signal has changed: the new value is in the arena, the processes it
    /// wakes are scheduled and every listener has recorded it. `cbValueChange`.
    virtual void valueChanged(std::uint32_t signalIndex) = 0;

    /// The earliest time anything the application scheduled is due, or `NoTime`.
    ///
    /// Taken into account when the kernel decides whether there is anything left to do,
    /// because a pending timed callback keeps a simulation running: a testbench written in
    /// Python is a design that schedules nothing of its own.
    [[nodiscard]] virtual SimTime nextTimedEvent() const = 0;

    /// Called when time has advanced to `time`, before anything at that time runs.
    /// `cbNextSimTime`.
    virtual void timeAdvanced(SimTime time) = 0;

    /// Runs everything due at or before `time`: delayed writes, then `cbAfterDelay` and
    /// `cbAtStartOfSimTime`.
    virtual void runTimedEvents(SimTime time) = 0;

    [[nodiscard]] virtual bool hasReadWrite() const = 0;

    /// Runs the pending `cbReadWriteSynch` callbacks, once the slot has settled.
    virtual void runReadWrite() = 0;

    [[nodiscard]] virtual bool hasReadOnly() const = 0;

    /// Runs the pending `cbReadOnlySynch` callbacks, before the Postponed region.
    virtual void runReadOnly() = 0;
};

/// Tunable kernel behaviour.
struct KernelOptions {
    /// Delta cycles allowed at one timestamp before the run is abandoned.
    ///
    /// The limit has to exist: a delta loop with no diagnostic is one of the most frustrating
    /// failures a user can hit.
    std::uint32_t deltaLimit = 10'000;

    TimeScale timeScale;

    /// Where `$display` and the severity tasks write. Null means the standard output.
    std::ostream* transcript = nullptr;

    /// What every random draw starts from (DESIGN_GUIDE.md section 3.8): `$random`'s own
    /// seed, and what each process's `$urandom` generator is derived from. Zero by default,
    /// which is where other simulators start `$random` too.
    std::uint32_t seed = 0;

    /// Calls one process may have in progress at once, in a design where a call can reach itself
    /// again. The call stack is laid out for this many when the kernel is built, and a run that
    /// would go deeper stops with a diagnostic naming the subprogram, as the delta limit stops a
    /// design that does not settle. A design that cannot recurse is given exactly the stack its
    /// deepest chain of calls needs, and no limit.
    std::uint32_t callDepthLimit = 10'000;

    /// The severity, as a position of VHDL's `severity_level`, at which a report or a failed
    /// assertion ends the run with a failing status: failure, as GHDL's default has it, unless
    /// `--assert-level` says otherwise; 4 never ends it.
    std::uint8_t stopSeverity = 3;
};

/// The body the interpreter is running and what it is bound to: a process's own, or that of a
/// subprogram the process called.
struct Activation {
    const codegen::CompiledBody* body = nullptr;

    /// Word offset of the body's registers in the kernel's frame storage.
    std::uint32_t frameOffset = 0;

    std::span<const std::uint32_t> objectSignals;
    std::span<const std::uint32_t> writeSignals;
    std::span<const WriteRoute> writeRoutes;

    /// For each of the body's call sites, the bound subprogram it calls.
    std::span<const std::uint32_t> calls;
};

/// The simulation kernel: one time wheel, one event queue, one arena.
///
/// There is exactly one of each whatever the languages — VHDL and SystemVerilog are not simulated
/// by separate engines that synchronise — so nothing here is specific to a source language. The
/// kernel sees compiled bodies, signals and slots, and has no idea which frontend produced them.
///
/// The kernel is single-threaded, which is the plan and not a fallback: a cache-friendly
/// single-threaded loop outperforms a naively parallel one, and determinism comes free.
class Kernel {
public:
    /// `image` and `diagnostics` must outlive the kernel.
    Kernel(const Image& designImageToRun, DiagnosticEngine& engine,
           KernelOptions kernelOptions = {});

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    Kernel(Kernel&&) = delete;
    Kernel& operator=(Kernel&&) = delete;
    ~Kernel();

    /// Writes time-zero values into the arena and schedules the processes that start at
    /// time zero. Must be called before `run`.
    void initialize();

    /// Runs until the design finishes, runs out of work, or reaches `until`.
    ///
    /// `until` is inclusive: events *at* `until` are executed, and the next timestamp
    /// beyond it is not. That matches what `run 100ns` means in every tool with this
    /// command, and it is what makes two consecutive `run` calls continue rather than
    /// skip a slot.
    RunStatus run(SimTime until = NoTime);

    [[nodiscard]] SimTime time() const noexcept { return currentTime; }

    [[nodiscard]] RunStatus status() const noexcept { return lastStatus; }

    /// The exit status `$finish` or `$fatal` asked for: 0 unless `$fatal` ran.
    [[nodiscard]] int exitStatus() const noexcept { return finishStatus; }

    [[nodiscard]] Arena& arena() noexcept { return stateArena; }

    [[nodiscard]] const Arena& arena() const noexcept { return stateArena; }

    [[nodiscard]] const Image& image() const noexcept { return *designImage; }

    // -----------------------------------------------------------------------
    // Operations a front end performs on a stopped kernel
    // -----------------------------------------------------------------------
    //
    // These are `examine`, `force` and `deposit`, which the GUI and VPI both use. They are on
    // the kernel rather than above it, because a second implementation of "write a signal
    // from outside" would be a second set of semantics.
    //
    // All of them are meant to be called between runs. Calling one from inside a process
    // would write state the running process may be holding a view of.

    /// Writes `value` into a signal now, waking whatever watches it.
    ///
    /// `deposit` in the Modelsim vocabulary: the value lands immediately and the next thing
    /// that drives the signal overwrites it. A deposit onto a forced signal does nothing,
    /// because a force outranks everything until it is released.
    void deposit(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// Holds `signalIndex` at `value` until `release`.
    ///
    /// Every subsequent write to the signal — procedural, non-blocking or another deposit —
    /// is discarded while the force stands, which is what distinguishes a force from a
    /// deposit and what makes it useful for pinning a reset or a mode bit. It is the design's
    /// own `force` held from outside, so it replaces one the design is holding.
    ///
    /// Forcing costs one branch on the signal write path and nothing at all until the first
    /// override in a run: the table is allocated when it is first needed, so nothing allocates on
    /// the hot path.
    void force(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// Releases a forced signal, returning false if it was not forced.
    ///
    /// A procedural `assign` the force stood over takes the signal back, by its value evaluated
    /// again. Otherwise the bits whose drivers keep their values, those of a signal a `force` in
    /// the design names, are resolved again now, and a net's undriven bits go back to their
    /// kind's undriven value (IEEE 1800 section 10.6.2). Any other bit stays where the force left
    /// it until something writes it again.
    bool release(std::uint32_t signalIndex);

    [[nodiscard]] bool isForced(std::uint32_t signalIndex) const noexcept;

    /// Registers a listener, which must outlive the kernel or be removed first.
    ///
    /// More than one is supported, and delivered to in registration order, because that is what the
    /// design calls for rather than a generalisation for its own sake: a batch run writes a VCD and
    /// an FST at once, and the GUI attaches and detaches while a simulation is running.
    void addListener(ValueChangeListener& newListener);

    /// Removes the first registration of `listener`. Does nothing if it was never registered.
    void removeListener(ValueChangeListener& listener);

    /// Redirects `$display` and the severity tasks, or restores the standard output with null.
    ///
    /// Changeable after construction because who is listening changes: a served kernel sends
    /// its output to the attached client, and the same kernel run from a script sends it to the
    /// transcript. The alternative — a kernel per destination — would mean a client and a
    /// script driving two different simulations.
    void setTranscript(std::ostream* stream) noexcept { options.transcript = stream; }

    /// Registers the source polled between time slots, or clears it with a null pointer.
    ///
    /// `source` must outlive the kernel or be cleared first. One rather than a list: the
    /// question has a single answer, and there is exactly one thing — the process driving the
    /// kernel — entitled to give it.
    void setInterrupt(RunInterrupt* source) noexcept { interrupt = source; }

    /// Registers the PLI application's hooks, or clears them with a null pointer.
    ///
    /// `hooks` must outlive the kernel or be cleared first. One rather than a list, because
    /// one VPI implementation serves every application a process loads.
    void setPliHooks(PliHooks* hooks) noexcept { pliHooks = hooks; }

    /// Ends the simulation as `$finish` does, once whatever is running returns.
    ///
    /// `vpi_control(vpiFinish)`. The exit status is untouched, so a run an application
    /// finishes ends successfully unless something else failed.
    void requestFinish() noexcept { finishRequested = true; }

    /// Stops the run at the next time-slot boundary, as a `RunInterrupt` would.
    ///
    /// `vpi_control(vpiStop)`. The boundary is the one a run can be resumed from with the
    /// same result, which is the whole reason `RunInterrupt` polls there.
    void requestStop() noexcept { stopRequested = true; }

    /// The unit every time the kernel reports is counted in.
    [[nodiscard]] TimeScale timeScale() const noexcept { return options.timeScale; }

    /// Total instructions executed, for the benchmark harness.
    [[nodiscard]] std::uint64_t executedInstructions() const noexcept { return instructionCount; }

    /// Total delta cycles across the whole run, for the benchmark harness and for
    /// diagnosing a design that almost fails to settle.
    [[nodiscard]] std::uint64_t totalDeltaCycles() const noexcept { return deltaTotal; }

    /// The waits armed on `signalIndex` by `@` and `wait`, stale ones not yet swept included.
    [[nodiscard]] std::size_t armedWaits(std::uint32_t signalIndex) const noexcept {
        return edgeWaiters[signalIndex].waiters.size();
    }

    // -----------------------------------------------------------------------
    // Operations the interpreter performs through the kernel
    // -----------------------------------------------------------------------
    //
    // Public because the interpreter is a separate class rather than because anything
    // else should call them. They are the kernel's contract with the execution engine,
    // and they are what a JIT will call too.

    /// Returns a mutable view of a process's register frame.
    [[nodiscard]] eir::BitRef frameAt(std::uint32_t wordOffset, std::uint32_t bitWidth) noexcept {
        return {frameStorage.data() + wordOffset, bitWidth};
    }

    [[nodiscard]] eir::ConstBitRef frameAt(std::uint32_t wordOffset,
                                           std::uint32_t bitWidth) const noexcept {
        return {frameStorage.data() + wordOffset, bitWidth};
    }

    /// Writes a whole signal, detecting change and waking its subscribers, unless an override
    /// holds it.
    void writeSignal(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// As `writeSignal`, past any override: the write an override itself makes.
    void changeSignal(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// True when a procedural `assign` or a `force` holds the signal, whose other writes are
    /// then discarded.
    [[nodiscard]] bool isOverridden(std::uint32_t signalIndex) const noexcept;

    /// Starts the procedural `assign`, or the `force`, of `signalIndex` that process
    /// `processIndex` runs, held by the override process its definition numbers `holder`, and
    /// writes `value` unless a force stands over the assign (IEEE 1800 section 10.6).
    void startOverride(std::uint32_t processIndex, std::uint32_t holder, std::uint32_t signalIndex,
                       eir::ConstBitRef value, bool force);

    /// Ends the procedural `assign` of `signalIndex`, which keeps its value.
    void deassign(std::uint32_t signalIndex);

    /// Writes an override's value evaluated again, if `processIndex` still holds what it was
    /// started as: the force, or the assign that no force stands over.
    void reassert(std::uint32_t processIndex, std::uint32_t signalIndex, eir::ConstBitRef value);

    /// Writes what a released signal's drivers say, as `release` describes.
    void restoreDriven(std::uint32_t signalIndex);

    /// Writes `value` through the driver of `route` whose range holds bit `lowBit` of the
    /// signal, and resolves what that changes (DESIGN_GUIDE.md section 3.7).
    ///
    /// Only the segments that driver covers are resolved, and the result reaches the signal
    /// through `writeSignalPart`, so a contended bit wakes nothing unless its resolved value
    /// moves. A write that lands in no driver's range is outside the signal and is dropped,
    /// as a write through an out-of-range select is.
    void writeThroughDrivers(const WriteRoute& route, eir::ConstBitRef value, std::uint64_t lowBit);

    /// Writes `value` into a signal starting at bit `lowBit`, leaving the rest alone.
    void writeSignalPart(std::uint32_t signalIndex, eir::ConstBitRef value, std::uint64_t lowBit);

    /// Records a non-blocking assignment, to be applied when the NBA region runs.
    void scheduleNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value);

    void scheduleNonBlockingPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                                 std::uint64_t lowBit);

    /// Suspends a process for `delay` time units, resuming at `resumePc`.
    ///
    /// A zero delay goes to the Inactive region, which is what `#0` means: the process
    /// resumes in this timestamp but after everything currently Active
    /// (DESIGN_GUIDE.md section 3.3).
    void suspendForTime(std::uint32_t processIndex, SimTime delay, std::uint32_t resumePc);

    /// How a delayed write treats the ones before it.
    enum class DelayKind : std::uint8_t {
        /// A continuous assignment's delay, and a module path's by default (IEEE 1800 section
        /// 10.3.3): it replaces the write the same store of the same process scheduled before,
        /// if that has not happened yet, so a pulse shorter than the delay never reaches the
        /// target. When it happens it is a write through the process's route, as a Store.
        Inertial,
        /// A module path's whose pulse limits are zero: as Inertial, replacing nothing.
        Transport,
        /// A nonblocking `a <= #d b`: each update happens, joining the NBA region of its time
        /// step.
        NonBlocking,
    };

    /// Schedules a write of `value` to `signalIndex` `delay` time units from now, through the
    /// route at `position` of process `processIndex`: of the whole signal, or of the bits from
    /// `lowBit` up as wide as the value. An inertial write replaces only the one its store made
    /// at the same bit.
    void scheduleDelayedWrite(std::uint32_t processIndex, std::uint32_t position,
                              std::uint32_t signalIndex, eir::ConstBitRef value, SimTime delay,
                              DelayKind kind, std::optional<std::uint32_t> lowBit = std::nullopt);

    /// True when `signalIndex` changed in the update the current simulation cycle began with:
    /// VHDL's 'event.
    [[nodiscard]] bool changedThisCycle(std::uint32_t signalIndex) const noexcept {
        return lastChange[signalIndex] == cycle;
    }

    /// True when `signalIndex` had a transaction or changed in the update the current simulation
    /// cycle began with: VHDL's 'active.
    [[nodiscard]] bool activeThisCycle(std::uint32_t signalIndex) const noexcept {
        return lastActive[signalIndex] == cycle;
    }

    /// The value `signalIndex` had before its last change, or its value when it has never changed:
    /// VHDL's 'last_value. Kept only for the signals some body reads it of.
    [[nodiscard]] eir::ConstBitRef lastValue(std::uint32_t signalIndex) const;

    /// Adds a transaction of `value` to the projected waveform of the driver a VHDL process has
    /// through its write position `position`, `delay` from now (IEEE 1076 section 10.5.2.2).
    ///
    /// The driver's transactions at or after the new one's time are deleted first. An `inertial`
    /// one also deletes those within `delay` before it, but for a run of transactions of its
    /// value immediately before it: a pulse shorter than the delay never reaches the signal.
    ///
    /// A transaction for now takes effect in the next delta cycle, after every process of this
    /// one has run, which is when a non-blocking update does. A later one takes effect when its
    /// time step begins, before any process runs in it, which is VHDL's signal update phase
    /// mapped onto the start of the Active region (DESIGN_GUIDE.md section 3.5).
    ///
    /// `value` is the bits from `lowBit` up that the assignment gives a value, all of the signal
    /// or part of it: each scalar subelement has a driver of its own, so a transaction deletes
    /// only what is pending for the bits it assigns.
    void drive(std::uint32_t processIndex, std::uint32_t position, std::uint32_t signalIndex,
               eir::ConstBitRef value, std::uint32_t lowBit, SimTime delay, bool inertial);

    /// Suspends a process until `edge` occurs on `signalIndex`.
    void suspendForEdge(std::uint32_t processIndex, std::uint32_t signalIndex, eir::EdgeKind edge,
                        std::uint32_t resumePc);

    /// Suspends `processIndex` until any entry of its event list `listIndex` happens.
    void suspendForEvents(std::uint32_t processIndex, std::uint32_t listIndex,
                          std::uint32_t resumePc);

    /// Suspends `processIndex` until any entry of its event list `listIndex` happens or `delay`
    /// passes, whichever comes first.
    void suspendForEventsFor(std::uint32_t processIndex, std::uint32_t listIndex,
                             std::uint32_t resumePc, SimTime delay);

    /// Performs a system task call of `activation`'s body, for process `processIndex`.
    void runSystemTask(std::uint32_t processIndex, const Activation& activation,
                       std::uint32_t sysCallIndex);

    /// Performs a system function call, writing its result to `result`.
    void runSystemFunction(std::uint32_t processIndex, const Activation& activation,
                           std::uint32_t sysCallIndex, eir::BitRef result);

    /// Fills the frame at `frame` with what `body`'s registers start as, which is its frame image:
    /// zero, but for the constants codegen placed there.
    void startFrame(std::uint32_t frame, const codegen::CompiledBody& body) noexcept {
        std::ranges::copy(body.frameImage, frameStorage.begin() + frame);
    }

    /// Starts the call `callIndex` of `caller`'s body: lays the callee's registers out on the
    /// call stack, copies the arguments in, and returns the callee's activation, the caller's
    /// being kept to return to at `resumePc`. Returns nothing having stopped the run when the
    /// call would go deeper than the stack holds.
    std::optional<Activation> enterCall(std::uint32_t processIndex, const Activation& caller,
                                        std::uint32_t callIndex, std::uint32_t resumePc);

    /// Returns from the innermost call to the activation that made it, copying the result and
    /// the `out` and `inout` parameters back, and returns that activation and the instruction
    /// to resume it at. Returns nothing when no call is in progress: the body returning is the
    /// process's own.
    std::optional<std::pair<Activation, std::uint32_t>> leaveCall(const Activation& callee);

    /// Abandons every call in progress, for a process that stops the run from inside one.
    void unwindCalls() noexcept;

private:
    /// Runs one timestamp to completion, iterating the regions as IEEE 1800 orders them.
    /// Returns false when the run must stop.
    bool runTimeSlot();

    /// Drains one region completely, executing each process.
    void drain(Region region);

    /// Applies every pending non-blocking update.
    void applyNonBlockingUpdates();

    /// Advances to the next scheduled time. Returns false when there is nothing left or
    /// the limit was reached.
    bool advanceTime(SimTime until);

    /// Schedules `processIndex` into Active from the start of its body, unless it is
    /// already queued.
    void wake(std::uint32_t processIndex);

    /// Schedules `processIndex` into Active to continue at `resumePc`.
    void resume(std::uint32_t processIndex, std::uint32_t resumePc);

    /// Where a process goes when it resumes: Active, or Postponed for a VHDL postponed process.
    [[nodiscard]] Region resumeRegion(std::uint32_t processIndex) const;

    /// Queues `processIndex`, which is not queued, to run: a port's connection with the others
    /// due in this update, any other process in its region.
    void enqueue(std::uint32_t processIndex);

    /// Runs the ports' connections due and applies what they drive, again until none is due, all
    /// in the delta cycle under way (eir::Process::connectsPort).
    void runConnections();

    /// Counts one delta cycle and enforces the limit. Returns false when the limit was
    /// exceeded and the run must stop.
    bool countDelta();

    /// Runs the design's `final` blocks, once.
    void runFinalProcesses();

    /// True when the PLI application has something to run at the current time.
    [[nodiscard]] bool pliWorkDue() const;

    [[nodiscard]] bool stopping() const noexcept { return finishRequested || errorRequested; }

    [[nodiscard]] bool halted() const noexcept { return stopping(); }

    /// Wakes whatever is waiting on a transition of `signalIndex`.
    /// Wakes and resumes what the change of `signalIndex` concerns. `before` and `after` are its
    /// lowest bit either side of the write; `previous` is its whole old value when the signal is
    /// windowed and empty otherwise, and `current` its new one.
    void notifyChange(std::uint32_t signalIndex, eir::Logic before, eir::Logic after,
                      eir::ConstBitRef previous, eir::ConstBitRef current);

    /// Copies a windowed signal's value before a write changes it, and returns the copy; returns
    /// an empty view for a signal nothing watches by window.
    eir::ConstBitRef keepPrevious(const Signal& signal, eir::ConstBitRef target);

    /// Records that `signalIndex` is about to change from `current`: the cycle it changes in, and,
    /// the first time in that cycle, the value it had, for a signal whose 'last_value is read.
    void recordEvent(std::uint32_t signalIndex, eir::ConstBitRef current);

    /// The cycle each signal last changed in, or `NeverChanged`, for VHDL's 'event; and the cycle
    /// now, which begins with each batch of updates: a delta cycle's, or a time step's.
    std::vector<std::uint64_t> lastChange;

    /// The cycle each signal last had a transaction or a change in, for 'active.
    std::vector<std::uint64_t> lastActive;
    std::uint64_t cycle = 0;
    static constexpr std::uint64_t NeverChanged = ~std::uint64_t{0};

    /// The value each signal had before its last change, for those signals some body reads
    /// 'last_value of: the Word offset of its copy in `lastValues`, or `NoLastValue`.
    std::vector<std::uint32_t> lastValueOffset;
    std::vector<eir::Word> lastValues;
    static constexpr std::uint32_t NoLastValue = 0xFFFF'FFFFU;

    /// Resolves every segment `driverIndex` covers and writes the result into its signal.
    void resolveDriver(std::uint32_t driverIndex);

    /// Resolves `segment`'s drivers into `acc`, on behalf of process `processIndex`. False when a
    /// resolution function's call stopped the run.
    bool resolveSegment(std::uint32_t processIndex, const elab::Segment& segment, eir::BitRef acc);

    /// Gives each VHDL resolved signal the value its drivers' initial values resolve to, before
    /// the run begins.
    void resolveInitialValues();

    /// Resolves `segment`, of a VHDL resolved signal, into `acc` with its resolution function,
    /// called for each subelement with its drivers' values, the first driver's left (IEEE 1076
    /// section 14.7.3.2). False when a call stopped the run.
    bool resolveByFunction(std::uint32_t processIndex, const elab::Segment& segment,
                           eir::BitRef acc);

    /// Calls the resolution function bound as `subprogram` with `drivers` as its array and
    /// leaves its result in `resolved`, on behalf of process `processIndex`. False when the call
    /// stopped the run.
    bool callResolution(std::uint32_t processIndex, std::uint32_t subprogram,
                        eir::ConstBitRef drivers, eir::BitRef resolved);

    /// Reports the delta limit being exceeded, naming what is still active.
    void reportDeltaLimit();

    [[nodiscard]] std::ostream& transcriptStream() const;

    const Image* designImage;
    DiagnosticEngine& diagnostics;
    KernelOptions options;

    Arena stateArena;

    /// Every process's registers, then the call stack.
    std::vector<eir::Word> frameStorage;

    /// A call in progress: the activation that made it, the call it made, and where to resume.
    struct CallInProgress {
        Activation caller;
        std::uint32_t callIndex = 0;
        std::uint32_t resumePc = 0;
    };

    /// The calls in progress, innermost last. Empty whenever no process is running, since a
    /// subprogram never suspends; reserved for the deepest the stack allows, so that a call
    /// does not allocate.
    std::vector<CallInProgress> callsInProgress;

    /// Where the call stack starts in `frameStorage`, and where its next frame goes.
    std::uint32_t callStackBase = 0;
    std::uint32_t callStackTop = 0;

    /// How many calls the stack holds at once.
    std::uint32_t callDepthCapacity = 0;
    Scheduler scheduler;

    /// Mutable per-process state. A copy of the image's list, because `resumePc` and
    /// `queued` change as the simulation runs and the image is shared and const.
    std::vector<ProcessState> processes;

    /// What holds each signal's overrides (DESIGN_GUIDE.md section 3.7): the override process of
    /// the procedural `assign` holding it and of the `force`, by process index, `FromOutside` for
    /// a force from the GUI or an application, or `NoHolder`. Empty while nothing has been
    /// overridden.
    ///
    /// Empty rather than sized at construction so that the check on the signal write path is
    /// a test of one size against zero for the whole of an ordinary run.
    struct Overrides {
        std::uint32_t assign = NoHolder;
        std::uint32_t force = NoHolder;

        [[nodiscard]] bool held() const noexcept { return assign != NoHolder || force != NoHolder; }
    };

    std::vector<Overrides> overrides;
    static constexpr std::uint32_t NoHolder = 0xFFFF'FFFFU;
    static constexpr std::uint32_t FromOutside = 0xFFFF'FFFEU;

    /// The process the interpreter is running, which its own writes do not wake, or
    /// `NoProcess` between processes.
    std::uint32_t runningProcess = NoProcess;
    static constexpr std::uint32_t NoProcess = 0xFFFF'FFFFU;

    /// Scratch for resolution, sized once for the widest driver: the resolved range, the segment
    /// being folded, and the driver being folded into it. Resolution runs on the write path, which
    /// must not allocate.
    std::vector<eir::Word> resolvedScratch;

    std::vector<eir::Word> segmentScratch;
    std::vector<eir::Word> driverScratch;

    /// The weak class's resolution of a segment, and one driver's part of a class.
    std::vector<eir::Word> weakScratch;
    std::vector<eir::Word> classScratch;

    /// A windowed signal's old value, kept across its write for the comparison, sized once
    /// for the widest windowed signal.
    std::vector<eir::Word> windowScratch;

    /// A file the design opened with `$fopen`: by descriptor, `0x8000'0000` with a number
    /// above the standard streams' 0 to 2, or by one channel bit of a multichannel descriptor
    /// when it was opened with no mode (IEEE 1800 section 21.3.1).
    ///
    /// A file opened for reading, as VHDL's are, has `input` rather than `stream`.
    struct OpenFile {
        std::uint32_t descriptor = 0;
        std::unique_ptr<std::ostream> stream;
        std::unique_ptr<std::istream> input;
    };

    std::vector<OpenFile> openFiles;
    std::uint32_t nextFileNumber = 3;
    std::uint32_t nextChannel = 1;

    /// The heap of access values: what VHDL allocates, std.textio's lines so far, by handle. Handle
    /// `n` is entry `n - 1`, and 0 is `null`; a freed entry is empty until an allocation reuses it,
    /// the last freed first, so that a run allocates the same handles every time.
    std::vector<std::optional<std::string>> heap;
    std::vector<std::uint64_t> freedHandles;

    /// Allocates a line holding `text` and returns its handle.
    std::uint64_t allocateLine(std::string text);

    /// The line `handle` designates, or null for `null` and for one freed.
    std::string* lineAt(std::uint64_t handle);

    /// Frees the line `handle` designates; freeing `null` does nothing.
    void freeLine(std::uint64_t handle);

    /// Runs `$readmemh` or `$readmemb`, whose arguments are the file name and the optional
    /// start and end addresses.
    void readMemory(std::span<const std::uint32_t> objectSignals,
                    const codegen::SysTaskDescriptor& descriptor,
                    std::span<const FormatArgument> arguments);

    /// Opens `name` for `$fopen`, or for VHDL's `file_open` with `r` to read it, and returns its
    /// descriptor or 0.
    std::uint32_t openFile(const std::string& name, const std::string& mode,
                           SourceLocation location);

    /// Writes `text` to every file and standard stream `descriptor` names. `task` and
    /// `location` are for the warning a descriptor that is not open gets.
    void writeToDescriptor(std::uint32_t descriptor, std::string_view text, std::string_view task,
                           SourceLocation location);

    /// The open file `descriptor` names on its own, or nothing: a standard stream, a
    /// multichannel descriptor and one not open are not.
    OpenFile* fileFor(std::uint32_t descriptor);

    /// `$random`'s seed when it is called without one of its own.
    std::int32_t randomSeed = 0;

    /// A write scheduled by `scheduleDelayedWrite`, waiting for its time.
    ///
    /// Each holds its own copy of the value. The time wheel allocates per entry as it is, so a pool
    /// would save nothing until both move to a calendar queue.
    struct DelayedWrite {
        std::uint32_t processIndex = 0;
        std::uint32_t position = 0;
        std::uint32_t signalIndex = 0;
        bool nonBlocking = false;

        /// A transaction of a VHDL driver, applied only if still on its projected waveform.
        bool transaction = false;
        std::uint64_t serial = 0;
        eir::BitVector value;

        /// The bit a write of part of the signal starts at, or nothing for the whole of it.
        std::optional<std::uint32_t> lowBit;
    };

    /// Pending delayed writes by time, in the order they were scheduled within one.
    std::map<SimTime, std::vector<DelayedWrite>> delayedWrites;

    /// The pending inertial write of each (process, write position, low bit), by serial and
    /// time, so that the next one can take it out of `delayedWrites`. The low bit of a whole
    /// signal's write is `WholeSignal`.
    struct PendingInertial {
        std::uint64_t serial = 0;
        SimTime time = 0;
    };

    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, PendingInertial>
        pendingInertial;
    static constexpr std::uint32_t WholeSignal = 0xFFFF'FFFFU;
    std::uint64_t nextWriteSerial = 0;

    /// Performs every delayed write due at the current time.
    void applyDelayedWrites();

    /// A transaction on the projected waveform of a VHDL driver. One for now is also in
    /// `deltaTransactions`, and a later one in `delayedWrites`, both by its serial.
    struct Transaction {
        SimTime time = 0;
        std::uint64_t serial = 0;

        /// The bits of the signal it assigns: from `lowBit`, as wide as `value`.
        std::uint32_t lowBit = 0;
        eir::BitVector value;

        [[nodiscard]] std::uint32_t endBit() const noexcept { return lowBit + value.width(); }
    };

    /// Each VHDL driver's projected waveform, in time order, indexed by `driverBase` of its
    /// process plus its write position. Flat, so that finding one costs an addition.
    std::vector<std::vector<Transaction>> projectedWaveforms;
    std::vector<std::uint32_t> driverBase;

    /// The transactions due in the next delta cycle, in the order they were made.
    struct DeltaTransaction {
        std::uint32_t processIndex = 0;
        std::uint32_t position = 0;
        std::uint32_t signalIndex = 0;
        std::uint64_t serial = 0;
    };

    std::vector<DeltaTransaction> deltaTransactions;

    /// A VHDL process's timeout: when it falls, the process resumes at `resumePc` unless what
    /// else it waits on resumed it first, which advanced its wait generation past `generation`.
    struct Timeout {
        std::uint32_t processIndex = 0;
        std::uint32_t generation = 0;
        std::uint32_t resumePc = 0;
    };

    /// Resumes the process of `timeout` unless it has already resumed.
    void resumeTimedOut(const Timeout& timeout);

    /// Takes back the timeout of `processIndex`, which what else it waited on resumed first, so
    /// that its time is not a step the run stops at.
    void cancelTimeout(std::uint32_t processIndex);

    /// The VHDL processes whose wait of no time, or timeout of none, resumes them in the next
    /// delta cycle, run with those its transactions wake (codegen::CompiledBody::resumesNextCycle).
    std::vector<Timeout> nextCycleResumes;

    /// The later timeouts of VHDL waits on events too, by the time each falls, and the time of
    /// each process's, or NoTime.
    std::map<SimTime, std::vector<Timeout>> timeouts;
    std::vector<SimTime> timeoutOf;

    /// The ports' connections whose sources changed, to run before the cycle's processes.
    std::vector<std::uint32_t> connectionsDue;

    /// Deletes `transaction`, a later one, from `delayedWrites`, taking its time step with it
    /// when nothing else is due then: a deleted transaction leaves no step behind.
    void unscheduleTransaction(const Transaction& transaction);

    /// Takes the transaction `serial` off its driver's projected waveform, or nothing when a later
    /// assignment has already deleted it.
    std::optional<Transaction> takeTransaction(std::uint32_t processIndex, std::uint32_t position,
                                               std::uint64_t serial);

    /// Writes what `transaction` assigns, through the process's route as a store does.
    void applyTransaction(std::uint32_t processIndex, std::uint32_t position,
                          std::uint32_t signalIndex, const Transaction& transaction);

    /// Whose a transaction is: the driver a process has through a write position, of a signal.
    struct DriverOf {
        std::uint32_t processIndex = 0;
        std::uint32_t position = 0;
        std::uint32_t signalIndex = 0;
    };

    /// Queues the transaction `serial` of `driver` for `time`: for the next delta cycle when that
    /// is now, and in the time wheel otherwise.
    void schedule(const DriverOf& driver, SimTime time, std::uint64_t serial);

    /// What remains of `transaction` on bits `lowBit` to `endBit` when an assignment deletes the
    /// rest, queued afresh as a transaction of its own.
    Transaction remainder(const DriverOf& driver, const Transaction& transaction,
                          std::uint32_t lowBit, std::uint32_t endBit);

    /// Applies the transactions due in this delta cycle, from the `first` made on.
    void applyDeltaTransactions(std::size_t first = 0);

    /// A write as a Store makes it, or of the bits from `lowBit` up as a StorePart does: through
    /// the process's drivers when its route has any.
    void writeRouted(std::uint32_t processIndex, std::uint32_t position, std::uint32_t signalIndex,
                     eir::ConstBitRef value, std::optional<std::uint32_t> lowBit = std::nullopt);

    /// Dynamic edge subscriptions created by `WaitEdge`, indexed by signal.
    ///
    /// Separate from the static subscriber list because these are consumed when they
    /// fire: a process that waited for one edge is not waiting for the next one until it
    /// asks again.
    struct EdgeWaiter {
        std::uint32_t processIndex = 0;
        eir::EdgeKind edge = eir::EdgeKind::AnyChange;
        std::uint32_t resumePc = 0;

        /// The process's wait generation when it armed this, which no longer matches once
        /// another entry of the same wait has fired.
        std::uint32_t generation = 0;

        /// The window watched; a width of 0 is the whole signal.
        std::uint32_t lowBit = 0;
        std::uint32_t width = 0;
    };

    /// One signal's dynamic subscriptions, and the length at which arming another first sweeps
    /// out the stale ones.
    struct EdgeWaiterList {
        std::vector<EdgeWaiter> waiters;
        std::size_t sweepAt = MinimumWaiterSweep;
    };

    static constexpr std::size_t MinimumWaiterSweep = 16;

    std::vector<EdgeWaiterList> edgeWaiters;

    /// Arms `waiter` on `signalIndex`.
    ///
    /// A stale subscription goes when its own signal next changes, and one whose signal never
    /// does would stay for the rest of the run: `forever @(posedge clk or posedge rst)` leaves
    /// one on `rst` for every clock. So a list that has doubled since it was last swept is
    /// swept before it grows, which bounds it by twice what is live, at a constant cost per
    /// wait.
    void armWaiter(std::uint32_t signalIndex, const EdgeWaiter& waiter);

    /// Non-owning, in registration order. Ordered rather than hash-based because what a listener
    /// records is observable and observable ordering must be reproducible.
    std::vector<ValueChangeListener*> listeners;

    /// Polled between time slots, or null. Non-owning.
    RunInterrupt* interrupt = nullptr;

    /// The loaded PLI application's hooks, or null. Non-owning.
    PliHooks* pliHooks = nullptr;

    SimTime currentTime = 0;
    RunStatus lastStatus = RunStatus::Quiescent;
    int finishStatus = 0;
    bool finishRequested = false;
    bool errorRequested = false;

    /// Set by `requestStop`, and cleared when the run it stops returns.
    bool stopRequested = false;

    bool finalProcessesRun = false;

    std::uint64_t instructionCount = 0;
    std::uint64_t deltaTotal = 0;

    /// How many process names a delta-limit report lists before abbreviating. Enough to
    /// identify a loop, few enough that the message stays readable.
    static constexpr unsigned MaxProcessesInDeltaReport = 10;

    friend class Interpreter;
};

} // namespace enzan::runtime
