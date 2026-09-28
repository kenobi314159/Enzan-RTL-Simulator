// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "runtime/Arena.h"
#include "runtime/Image.h"
#include "runtime/Scheduler.h"
#include "runtime/Time.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
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
/// Section 2.2 requires the GUI to be able to stop a running kernel, and requires that a
/// stalled or slow GUI never stall the kernel. Both fall out of polling rather than being
/// signalled: the kernel asks when it is between time slots and therefore in a state it can
/// be resumed from, and nothing outside the kernel ever touches its state.
///
/// The poll is deliberately at a time-slot boundary and not finer. All the state a run holds
/// lives in the scheduler and the arena, so stopping there and calling `run` again produces
/// exactly the results an uninterrupted run would have produced — which is what section 3.8
/// requires, and which would not be true of an interruption part-way through a region.
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
    /// for a write of the value a signal already held (section 6.2).
    virtual void onValueChange(std::uint32_t signalIndex, eir::ConstBitRef value) = 0;

    /// Called when simulated time advances, before anything at the new time runs.
    virtual void onTimeAdvance(SimTime time) = 0;

    /// `$dumpfile`. The default ignores it, because the file is normally chosen on the
    /// command line and a design that names one should not override that silently.
    virtual void onDumpFile([[maybe_unused]] std::string_view path) {}

    /// `$dumpvars`.
    virtual void onDumpVars() {}
};

/// Tunable kernel behaviour.
struct KernelOptions {
    /// Delta cycles allowed at one timestamp before the run is abandoned.
    ///
    /// Section 3.2 sets the default at 10,000 and requires the limit to exist: a delta
    /// loop with no diagnostic is one of the most frustrating failures a user can hit.
    std::uint32_t deltaLimit = 10'000;

    TimeScale timeScale;

    /// Where `$display` and the severity tasks write. Null means the standard output.
    std::ostream* transcript = nullptr;
};

/// The simulation kernel: one time wheel, one event queue, one arena.
///
/// Section 3.5 requires that there be exactly one of each even once VHDL arrives —
/// VHDL and SystemVerilog are not simulated by separate engines that synchronise — so
/// nothing here is specific to a source language. The kernel sees compiled bodies,
/// signals and slots, and has no idea which frontend produced them.
///
/// The kernel is single-threaded, which is the plan and not a fallback (section 6.4): a
/// cache-friendly single-threaded loop outperforms a naively parallel one, and
/// determinism comes free.
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
    // These are the `examine`, `force` and `deposit` of DESIGN_GUIDE.md section 10.4, which
    // are required from both Tcl and the GUI. They are on the kernel rather than in the
    // the kernel rather than above it, because a second implementation of
    // "write a signal from outside" would be a second set of semantics.
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
    /// deposit and what makes it useful for pinning a reset or a mode bit.
    ///
    /// Forcing costs one branch on the signal write path and nothing at all until the first
    /// force in a run: the table is allocated when it is first needed, so section 13.4's ban
    /// on hot-path allocation is not in question.
    void force(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// Releases a forced signal, returning false if it was not forced.
    ///
    /// The value stays where the force left it until something drives the signal again.
    /// Recomputing what the drivers would now say needs driver resolution, which is M5; until
    /// then a released signal keeps its value rather than reverting to a number Enzan would
    /// have had to invent.
    bool release(std::uint32_t signalIndex);

    [[nodiscard]] bool isForced(std::uint32_t signalIndex) const noexcept;

    /// Registers a listener, which must outlive the kernel or be removed first.
    ///
    /// More than one is supported, and delivered to in registration order, because that is what
    /// the design calls for rather than a generalisation for its own sake: a batch run writes a
    /// VCD and an FST at once, and section 2.2 requires the GUI to attach and detach while a
    /// simulation is running.
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

    /// Total instructions executed, for the benchmark harness.
    [[nodiscard]] std::uint64_t executedInstructions() const noexcept { return instructionCount; }

    /// Total delta cycles across the whole run, for the benchmark harness and for
    /// diagnosing a design that almost fails to settle.
    [[nodiscard]] std::uint64_t totalDeltaCycles() const noexcept { return deltaTotal; }

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

    /// Writes a whole signal, detecting change and waking its subscribers.
    void writeSignal(std::uint32_t signalIndex, eir::ConstBitRef value);

    /// Writes `value` into a signal starting at bit `lowBit`, leaving the rest alone.
    void writeSignalPart(std::uint32_t signalIndex, eir::ConstBitRef value, std::uint64_t lowBit);

    /// Records a non-blocking assignment, to be applied when the NBA region runs.
    void scheduleNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value);

    void scheduleNonBlockingPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                                 std::uint64_t lowBit);

    /// Suspends a process for `delay` time units, resuming at `resumePc`.
    ///
    /// A zero delay goes to the Inactive region, which is what `#0` means: the process
    /// resumes in this timestamp but after everything currently Active (section 3.3).
    void suspendForTime(std::uint32_t processIndex, SimTime delay, std::uint32_t resumePc);

    /// Suspends a process until `edge` occurs on `signalIndex`.
    void suspendForEdge(std::uint32_t processIndex, std::uint32_t signalIndex, eir::EdgeKind edge,
                        std::uint32_t resumePc);

    /// Performs a system task call.
    void runSystemTask(std::uint32_t processIndex, std::uint32_t sysCallIndex);

private:
    /// Runs one timestamp to completion, iterating the regions per section 3.3.
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

    /// Counts one delta cycle and enforces the limit. Returns false when the limit was
    /// exceeded and the run must stop.
    bool countDelta();

    /// Runs the design's `final` blocks, once.
    void runFinalProcesses();

    [[nodiscard]] bool stopping() const noexcept { return finishRequested || errorRequested; }

    [[nodiscard]] bool halted() const noexcept { return stopping(); }

    /// Wakes whatever is waiting on a transition of `signalIndex`.
    void notifyChange(std::uint32_t signalIndex, eir::Logic before, eir::Logic after);

    /// Reports the delta limit being exceeded, naming what is still active.
    void reportDeltaLimit();

    [[nodiscard]] std::ostream& transcriptStream() const;

    const Image* designImage;
    DiagnosticEngine& diagnostics;
    KernelOptions options;

    Arena stateArena;
    std::vector<eir::Word> frameStorage;
    Scheduler scheduler;

    /// Mutable per-process state. A copy of the image's list, because `resumePc` and
    /// `queued` change as the simulation runs and the image is shared and const.
    std::vector<ProcessState> processes;

    /// True for each signal a `force` is holding, or empty when nothing is forced.
    ///
    /// Empty rather than sized at construction so that the check on the signal write path is
    /// a test of one size against zero for the whole of an ordinary run.
    std::vector<bool> forcedSignals;

    /// Dynamic edge subscriptions created by `WaitEdge`, indexed by signal.
    ///
    /// Separate from the static subscriber list because these are consumed when they
    /// fire: a process that waited for one edge is not waiting for the next one until it
    /// asks again.
    struct EdgeWaiter {
        std::uint32_t processIndex = 0;
        eir::EdgeKind edge = eir::EdgeKind::AnyChange;
        std::uint32_t resumePc = 0;
    };

    std::vector<std::vector<EdgeWaiter>> edgeWaiters;

    /// Non-owning, in registration order. Ordered rather than hash-based because what a listener
    /// records is observable and section 3.8 makes observable ordering reproducible.
    std::vector<ValueChangeListener*> listeners;

    /// Polled between time slots, or null. Non-owning.
    RunInterrupt* interrupt = nullptr;

    SimTime currentTime = 0;
    RunStatus lastStatus = RunStatus::Quiescent;
    int finishStatus = 0;
    bool finishRequested = false;
    bool errorRequested = false;

    bool finalProcessesRun = false;

    std::uint64_t instructionCount = 0;
    std::uint64_t deltaTotal = 0;

    /// How many process names a delta-limit report lists before abbreviating. Enough to
    /// identify a loop, few enough that the message stays readable.
    static constexpr unsigned MaxProcessesInDeltaReport = 10;

    friend class Interpreter;
};

} // namespace enzan::runtime
