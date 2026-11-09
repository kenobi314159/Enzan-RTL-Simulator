// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Kernel.h"

#include "codegen/Bytecode.h"
#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "eir/Process.h"
#include "runtime/Format.h"
#include "runtime/Interpreter.h"
#include "runtime/ReadMem.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace enzan::runtime {

namespace bitops = eir::bitops;

std::string_view toString(RunStatus status) noexcept {
    switch (status) {
    case RunStatus::TimeLimitReached: return "time limit reached";
    case RunStatus::Quiescent: return "no events remaining";
    case RunStatus::Finished: return "finished";
    case RunStatus::Error: return "error";
    case RunStatus::Interrupted: return "interrupted";
    }
    return "error";
}

namespace {

/// True when the transition from `before` to `after` is a positive edge.
///
/// IEEE 1364 defines `posedge` as a transition *towards* 1, not merely 0 to 1: 0 to 1,
/// 0 to X, 0 to Z, X to 1 and Z to 1 all count. That matters on the first edge of a
/// clock that starts out X, which is every clock in every design at time zero.
bool isPosedge(eir::Logic before, eir::Logic after) noexcept {
    if (before == after) return false;
    return before == eir::Logic::Zero || after == eir::Logic::One;
}

bool isNegedge(eir::Logic before, eir::Logic after) noexcept {
    if (before == after) return false;
    return before == eir::Logic::One || after == eir::Logic::Zero;
}

/// The most characters a string of run-time length may hold: more than any VHDL integer range
/// spans but the whole of one, so that a mistaken bound is a failure and not an allocation
/// that fails.
constexpr std::uint64_t MaxStringLength = std::uint64_t{1} << 31;

/// The character whose code is the byte of `bits` from `lowBit` up; an unknown bit reads as 0.
char characterAt(eir::ConstBitRef bits, std::uint64_t lowBit) {
    eir::BitVector code(8);
    bitops::partSelectAt(code.mutableRef(), bits, lowBit);
    const eir::ConstBitRef known = code.ref();
    return static_cast<char>(known.aval()[0] & ~known.bval()[0] & 0xFFU);
}

/// Writes `character`'s code into the byte of `bits` from `lowBit` up.
void putCharacter(eir::BitRef bits, std::uint64_t lowBit, char character) {
    bitops::assignPartAt(
        bits, eir::BitVector::fromUInt64(static_cast<unsigned char>(character), 8).ref(), lowBit);
}

/// The words a windowed signal's old value needs, for the widest such signal.
std::size_t windowScratchWords(const Image& image) {
    std::uint32_t widest = 0;
    for (const Signal& signal : image.signals())
        if (signal.windowed) widest = std::max(widest, signal.bitWidth);
    return eir::storageWordsFor(widest);
}

/// The unit a call's `%t` arguments count, or nothing when they count the run's.
std::optional<TimeExponent> argumentUnit(const codegen::SysTaskDescriptor& descriptor) {
    if (descriptor.timeUnit == eir::SysTaskCall::RunUnit) return std::nullopt;
    return descriptor.timeUnit;
}

/// IEEE 1364's `$random`: the reference C code the standard gives for its distribution
/// functions, `rtl_dist_uniform`, over the whole 32-bit range. The seed advances as a linear
/// congruential generator and the draw is computed from it in floating point. Kept bit for bit,
/// because every simulator that follows it, Icarus Verilog among them, draws the same sequence
/// from the same seed.
std::int32_t ieeeRandom(std::int32_t& seed) {
    if (seed == 0) seed = 259'341'593;
    seed = static_cast<std::int32_t>((69069U * static_cast<std::uint32_t>(seed)) + 1U);

    const std::uint32_t bits = (static_cast<std::uint32_t>(seed) >> 9U) | 0x3f80'0000U;
    float single = 0;
    std::memcpy(&single, &bits, sizeof single);
    auto fraction = static_cast<double>(single);
    fraction += fraction * 0.00000011920928955078125;

    const double low = -2147483648.0;
    const double high = 2147483647.0;
    double draw = ((high - low) * (fraction - 1.0)) + low;
    draw = (draw + 2147483648.0) / 4294967295.0;
    draw = (draw * 4294967296.0) - 2147483648.0;
    return static_cast<std::int32_t>(draw >= 0 ? draw : draw - 1);
}

/// SplitMix64, for `$urandom`: small, fast and well distributed, which is all a process's
/// stimulus needs.
std::uint64_t splitMix(std::uint64_t& state) {
    state += 0x9e37'79b9'7f4a'7c15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return mixed ^ (mixed >> 31U);
}

/// A process's generator state from the seed and its name, which stay the same from run to
/// run whatever else the design does.
std::uint64_t processRandomState(std::uint32_t seed, std::string_view name) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    for (const char c : name) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x0000'0100'0000'01b3ULL;
    }
    return hash ^ (std::uint64_t{seed} * 0x9e37'79b9'7f4a'7c15ULL);
}

bool edgeMatches(eir::EdgeKind edge, eir::Logic before, eir::Logic after) noexcept {
    switch (edge) {
    case eir::EdgeKind::AnyChange:
        // The caller only calls this when something changed, so any change matches.
        return true;
    case eir::EdgeKind::Posedge: return isPosedge(before, after);
    case eir::EdgeKind::Negedge: return isNegedge(before, after);
    }
    return false;
}

/// How many calls the stack must hold at once: the deepest chain, or the limit where a call can
/// reach itself again and no chain is the deepest.
std::uint32_t callDepthFor(const CallStackShape& shape, std::uint32_t limit) {
    return shape.recursive ? limit : shape.depth;
}

/// The call stack's size in Words: the deepest chain's frames, or as many of the largest frame as
/// the limit allows calls.
std::size_t callStackWordsFor(const CallStackShape& shape, std::uint32_t limit) {
    return shape.recursive ? std::size_t{limit} * shape.largestFrame : shape.words;
}

} // namespace

Kernel::Kernel(const Image& designImageToRun, DiagnosticEngine& engine, KernelOptions kernelOptions)
    : designImage(&designImageToRun),
      diagnostics(engine),
      options(kernelOptions),
      stateArena(designImageToRun.arenaWords()),
      frameStorage(
          designImageToRun.frameWords()
              + callStackWordsFor(designImageToRun.callStackShape(), kernelOptions.callDepthLimit),
          0),
      callStackBase(designImageToRun.frameWords()),
      callStackTop(designImageToRun.frameWords()),
      callDepthCapacity(
          callDepthFor(designImageToRun.callStackShape(), kernelOptions.callDepthLimit)),
      processes(designImageToRun.processes().begin(), designImageToRun.processes().end()),
      resolvedScratch(eir::storageWordsFor(designImageToRun.elaboratedDesign().widestDriver()), 0),
      segmentScratch(resolvedScratch.size(), 0),
      driverScratch(resolvedScratch.size(), 0),
      weakScratch(resolvedScratch.size(), 0),
      classScratch(resolvedScratch.size(), 0),
      windowScratch(windowScratchWords(designImageToRun), 0),
      edgeWaiters(designImageToRun.signals().size()) {
    callsInProgress.reserve(callDepthCapacity);

    // One projected waveform per object each process writes, which is one per driver a VHDL
    // process has; a process of another language never fills its own.
    driverBase.reserve(processes.size());
    std::uint32_t drivers = 0;
    for (const ProcessState& state : processes) {
        driverBase.push_back(drivers);
        drivers += static_cast<std::uint32_t>(state.writeRoutes.size());
    }
    projectedWaveforms.resize(drivers);

    // A shadow of each signal whose 'last_value a body reads, found where it is read.
    const std::span<const Signal> signals = designImageToRun.signals();
    lastChange.assign(signals.size(), NeverChanged);
    lastActive.assign(signals.size(), NeverChanged);
    lastValueOffset.assign(signals.size(), NoLastValue);
    const auto keepLastValues = [&](std::uint32_t bodyIndex,
                                    std::span<const std::uint32_t> objectSignals) {
        for (const codegen::Instruction& instruction :
             designImageToRun.compiledDesign().body(bodyIndex).code) {
            if (instruction.opcode != eir::Opcode::ReadLastValue) continue;
            const std::uint32_t signalIndex = objectSignals[instruction.a];
            if (lastValueOffset[signalIndex] != NoLastValue) continue;
            lastValueOffset[signalIndex] = static_cast<std::uint32_t>(lastValues.size());
            const std::size_t words = eir::storageWordsFor(signals[signalIndex].bitWidth);
            lastValues.resize(lastValues.size() + (2 * words));
        }
    };
    for (const ProcessState& state : processes)
        keepLastValues(state.bodyIndex, state.objectSignals);
    for (const BoundSubprogram& bound : designImageToRun.subprograms())
        keepLastValues(bound.bodyIndex, bound.objectSignals);
}

Kernel::~Kernel() = default;

std::ostream& Kernel::transcriptStream() const {
    return options.transcript != nullptr ? *options.transcript : std::cout;
}

void Kernel::initialize() {
    designImage->initializeState(stateArena);
    resolveInitialValues();

    // Everything a previous run left behind goes with it, because a restart is this method
    // called a second time and a design put back to time zero must behave exactly
    // as it did the first time. A clock event still in the time wheel would resume at a
    // timestamp the design has not reached; a process still holding a resume point would
    // continue a simulation that no longer exists; an edge subscription would fire for an
    // edge nothing is waiting for any more. Each of those produces a design that runs and
    // gives a different answer, which is the worst possible outcome.
    scheduler.clear();
    delayedWrites.clear();
    pendingInertial.clear();
    deltaTransactions.clear();
    nextCycleResumes.clear();
    timeouts.clear();
    timeoutOf.assign(designImage->processes().size(), NoTime);
    connectionsDue.clear();
    for (std::vector<Transaction>& waveform : projectedWaveforms) waveform.clear();
    cycle = 0;
    processes.assign(designImage->processes().begin(), designImage->processes().end());
    for (ProcessState& state : processes)
        state.randomState = processRandomState(options.seed, state.name);
    randomSeed = static_cast<std::int32_t>(options.seed);
    // Files a previous run opened are closed, which flushes what it wrote, and are opened
    // again, from the start, by the run that follows.
    openFiles.clear();
    nextFileNumber = 3;
    nextChannel = 1;
    // The lines it allocated go too, so that the run that follows allocates the same handles.
    heap.clear();
    freedHandles.clear();
    scratch.clear();
    for (EdgeWaiterList& list : edgeWaiters) {
        list.waiters.clear();
        list.sweepAt = MinimumWaiterSweep;
    }
    std::ranges::fill(frameStorage, eir::Word{});
    for (const ProcessState& state : processes)
        startFrame(state.frameOffset, designImage->compiledDesign().body(state.bodyIndex));
    unwindCalls();

    // An override does not survive a restart either: a signal still pinned from the last run
    // would be state the user did not ask to keep.
    overrides.clear();

    finishRequested = false;
    errorRequested = false;
    stopRequested = false;
    finishStatus = 0;
    finalProcessesRun = false;
    lastStatus = RunStatus::Quiescent;

    currentTime = 0;

    // The ports' connections run first, and what they copy is where the signals they drive
    // start (IEEE 1076 section 14.7.5.2) rather than a change of them: what the copies woke is
    // forgotten, as nothing has run yet, and they wait on their sources as they do after.
    for (std::uint32_t processIndex = 0; processIndex < processes.size(); ++processIndex) {
        if (designImage->compiledDesign().body(processes[processIndex].bodyIndex).connectsPort)
            wake(processIndex);
    }
    runConnections();
    scheduler.clear();
    for (ProcessState& state : processes) state.queued = false;

    // Before its first change, a signal's 'last_value is its value.
    std::ranges::fill(lastChange, NeverChanged);
    std::ranges::fill(lastActive, NeverChanged);
    for (std::uint32_t signalIndex = 0; signalIndex < lastValueOffset.size(); ++signalIndex) {
        if (lastValueOffset[signalIndex] == NoLastValue) continue;
        const Signal& signal = designImage->signals()[signalIndex];
        bitops::copy(eir::BitRef(lastValues.data() + lastValueOffset[signalIndex], signal.bitWidth),
                     stateArena.at(signal.wordOffset, signal.bitWidth));
    }

    for (std::uint32_t processIndex = 0; processIndex < processes.size(); ++processIndex) {
        const codegen::CompiledBody& body =
            designImage->compiledDesign().body(processes[processIndex].bodyIndex);
        if (!body.startsAtTimeZero || body.connectsPort) continue;
        // A postponed process runs at initialization after the others and before the first
        // delta cycle (IEEE 1076 section 14.7.5.2), which Inactive is.
        if (body.postponed) {
            processes[processIndex].queued = true;
            scheduler.schedule(Region::Inactive, processIndex);
            continue;
        }
        wake(processIndex);
    }
}

RunStatus Kernel::run(SimTime until) {
    while (true) {
        // Asked here, between time slots, because that is the only point at which the whole of
        // the run's state is in the scheduler and the arena and nothing is half-done. Stopping
        // anywhere else would make resuming produce a different answer from running straight
        // through, and a result must not depend on where a run was paused.
        if (stopRequested || (interrupt != nullptr && interrupt->shouldInterrupt())) {
            stopRequested = false;
            lastStatus = RunStatus::Interrupted;
            return lastStatus;
        }

        // A finish asked for outside a time slot — by an application, from a callback that ran
        // before the run began — ends the run before anything else executes, exactly as one
        // asked for inside a slot ends it before the next.
        if (stopping()) {
            lastStatus = errorRequested ? RunStatus::Error : RunStatus::Finished;
            runFinalProcesses();
            return lastStatus;
        }

        if (!scheduler.allRegionsEmpty() || scheduler.hasNonBlockingUpdates()
            || !deltaTransactions.empty() || !nextCycleResumes.empty() || !connectionsDue.empty()
            || pliWorkDue()) {
            if (!runTimeSlot()) {
                lastStatus = errorRequested ? RunStatus::Error : RunStatus::Finished;
                runFinalProcesses();
                return lastStatus;
            }
        }

        // A stop asked for in the last slot is a stop of this run, which has ended anyway. Kept,
        // it would end the next run before that run had done anything.
        if (!advanceTime(until)) {
            stopRequested = false;
            return lastStatus;
        }
    }
}

bool Kernel::runTimeSlot() {
    // IEEE 1800's region iteration rules. Active is drained fully; only when Active,
    // Inactive and NBA are all empty does the slot move on to Observed, and the
    // Reactive family iterates by the same rule. Anything that schedules Active work
    // sends control back to the top.
    //
    // An application's timed events run first, before the Active region is drained, and its
    // read-write callbacks once everything else has settled.
    bool firstPass = true;
    while (true) {
        if (pliHooks != nullptr && pliHooks->nextTimedEvent() <= currentTime) {
            // On any pass but the first, what is due was scheduled during this slot with no
            // delay, and running it is a further iteration of the slot. Counting it keeps an
            // application that does that forever inside the delta limit.
            if (!firstPass && !countDelta()) return false;
            pliHooks->runTimedEvents(currentTime);
            if (halted()) return false;
        }
        firstPass = false;

        // Connections whose sources changed, as the step began or by what ran last, carry the
        // change on before anything reads it.
        runConnections();
        if (halted()) return false;

        drain(Region::Active);
        if (halted()) return false;
        if (!connectionsDue.empty()) continue;

        if (!scheduler.empty(Region::Inactive)) {
            scheduler.promote(Region::Inactive, Region::Active);
            if (!countDelta()) return false;
            continue;
        }

        // VHDL's transactions for the next delta cycle are applied with SystemVerilog's NBA
        // updates: either way, after every process of this iteration has run. The processes
        // waiting no time resume after them, in the cycle they begin.
        if (scheduler.hasNonBlockingUpdates() || !deltaTransactions.empty()
            || !nextCycleResumes.empty()) {
            ++cycle;
            applyNonBlockingUpdates();
            if (halted()) return false;
            applyDeltaTransactions();
            if (halted()) return false;
            runConnections();
            if (halted()) return false;
            for (const Timeout& timeout : std::exchange(nextCycleResumes, {}))
                resumeTimedOut(timeout);
            if (!countDelta()) return false;
            continue;
        }

        if (!scheduler.empty(Region::Observed)) {
            drain(Region::Observed);
            if (halted()) return false;
            continue;
        }

        if (!scheduler.empty(Region::Reactive)) {
            drain(Region::Reactive);
            if (halted()) return false;
            continue;
        }

        if (!scheduler.empty(Region::ReInactive)) {
            scheduler.promote(Region::ReInactive, Region::Reactive);
            if (!countDelta()) return false;
            continue;
        }

        if (!scheduler.empty(Region::ReNba)) {
            scheduler.promote(Region::ReNba, Region::Reactive);
            if (!countDelta()) return false;
            continue;
        }

        // Read-write synchronisation is the application's chance to write once the design has
        // settled, and whatever it writes has to settle in turn, so it sends control back to
        // the top like any other region that schedules work. It counts as a delta for the
        // same reason a `#0` does: an application that asks for read-write from read-write
        // forever is a delta loop, and the limit is what finds one.
        if (pliHooks != nullptr && pliHooks->hasReadWrite()) {
            pliHooks->runReadWrite();
            if (halted()) return false;
            if (!countDelta()) return false;
            continue;
        }

        break;
    }

    // Read-only synchronisation sees the slot's final values, before Postponed. Writes are
    // refused while it runs, so it cannot send control back.
    if (pliHooks != nullptr && pliHooks->hasReadOnly()) {
        pliHooks->runReadOnly();
        if (halted()) return false;
    }

    // Postponed is last: `$monitor` is read-only by definition, and a VHDL postponed process
    // may assign only after a delay (IEEE 1076 section 11.3).
    drain(Region::Postponed);
    return !halted();
}

bool Kernel::pliWorkDue() const {
    if (pliHooks == nullptr) return false;
    return pliHooks->nextTimedEvent() <= currentTime || pliHooks->hasReadWrite()
           || pliHooks->hasReadOnly();
}

bool Kernel::countDelta() {
    scheduler.countDelta();
    ++deltaTotal;

    if (scheduler.deltaCount() <= options.deltaLimit) return true;

    reportDeltaLimit();
    errorRequested = true;
    return false;
}

void Kernel::drain(Region region) {
    Interpreter interpreter(*this);

    while (!scheduler.empty(region)) {
        const std::uint32_t processIndex = scheduler.take(region);
        processes[processIndex].queued = false;

        runningProcess = processIndex;
        const ExecutionResult result = interpreter.run(processIndex);
        runningProcess = NoProcess;

        if (result == ExecutionResult::Stopped || stopping()) return;

        if (result != ExecutionResult::Completed) continue;

        const codegen::CompiledBody& body =
            designImage->compiledDesign().body(processes[processIndex].bodyIndex);

        // A bare `always` loop has nothing to wait for, so completing means going round
        // again. Counting that as a delta cycle is what lets the delta limit catch an
        // `always` block with no time control at all, which would otherwise spin in the
        // Active region forever without ever incrementing anything.
        if (body.kind == eir::ProcessKind::Always && body.sensitivity.empty()) {
            wake(processIndex);
            if (!countDelta()) return;
        }
    }
}

void Kernel::applyNonBlockingUpdates() {
    // Taken as a batch, because applying an update can append another: a write runs the
    // application's value-change callbacks, and one of those may write with an inertial delay.
    // That update belongs to the next NBA pass, not to this one.
    for (const NonBlockingUpdate& update : scheduler.takeNonBlockingUpdates()) {
        if (update.partial) {
            writeSignalPart(update.signalIndex, scheduler.takenValue(update), update.lowBit);
        } else {
            writeSignal(update.signalIndex, scheduler.takenValue(update));
        }
        if (stopping()) break;
    }
}

bool Kernel::advanceTime(SimTime until) {
    const SimTime next = std::min({scheduler.nextEventTime(),
                                   pliHooks != nullptr ? pliHooks->nextTimedEvent() : NoTime,
                                   delayedWrites.empty() ? NoTime : delayedWrites.begin()->first,
                                   timeouts.empty() ? NoTime : timeouts.begin()->first});

    // Something the application scheduled with no delay after the slot had settled — from a
    // read-only callback — is due now. It runs in a further slot at this time, and time has
    // not advanced, so nothing that reports an advance is told about one. That further slot
    // is a delta like any other, or an application doing it forever would never be stopped.
    if (next <= currentTime) {
        if (countDelta()) return true;
        lastStatus = RunStatus::Error;
        runFinalProcesses();
        return false;
    }

    if (next == NoTime) {
        lastStatus = RunStatus::Quiescent;
        runFinalProcesses();
        return false;
    }

    if (next > until) {
        lastStatus = RunStatus::TimeLimitReached;
        return false;
    }

    currentTime = next;
    scheduler.resetDeltaCount();

    for (ValueChangeListener* listener : listeners) listener->onTimeAdvance(currentTime);

    if (pliHooks != nullptr) pliHooks->timeAdvanced(currentTime);

    scheduler.releaseTime(next);
    if (const auto due = timeouts.find(next); due != timeouts.end()) {
        const std::vector<Timeout> fallen = std::move(due->second);
        timeouts.erase(due);
        for (const Timeout& timeout : fallen) {
            timeoutOf[timeout.processIndex] = NoTime;
            resumeTimedOut(timeout);
        }
    }
    ++cycle;
    applyDelayedWrites();
    return true;
}

void Kernel::runFinalProcesses() {
    // `final` blocks run once when the simulation ends, which is after the last time
    // slot rather than inside it. Scheduled and drained here so that they see the design's
    // last state and so that a `$display` in one appears at the end of the transcript.
    if (finalProcessesRun) return;
    finalProcessesRun = true;

    bool any = false;
    for (std::uint32_t processIndex = 0; processIndex < processes.size(); ++processIndex) {
        const codegen::CompiledBody& body =
            designImage->compiledDesign().body(processes[processIndex].bodyIndex);
        if (body.kind != eir::ProcessKind::Final) continue;

        processes[processIndex].resumePc = 0;
        processes[processIndex].queued = true;
        scheduler.schedule(Region::Active, processIndex);
        any = true;
    }

    if (!any) return;

    // `$finish` has already been requested by the time this runs, so the stop flags are
    // cleared for the duration; a `$finish` inside a `final` block has nothing left to
    // stop.
    const bool savedFinish = finishRequested;
    finishRequested = false;
    drain(Region::Active);
    finishRequested = savedFinish;
}

void Kernel::wake(std::uint32_t processIndex) {
    ProcessState& state = processes[processIndex];
    if (state.queued) return;

    // A process is not waiting on its event control while it runs, so what it writes then
    // cannot wake it. Without this an `always @*` that sets a temporary and reads it back —
    // `k = 0; ... k = a ^ b; y = k;`, which is ordinary Verilog — is sensitive to its own
    // writes, wakes itself on every run, and never lets the time slot end.
    if (processIndex == runningProcess) return;

    state.resumePc = 0;
    state.queued = true;
    enqueue(processIndex);
}

void Kernel::resume(std::uint32_t processIndex, std::uint32_t resumePc) {
    ProcessState& state = processes[processIndex];
    state.resumePc = resumePc;

    if (state.queued) return;

    state.queued = true;
    enqueue(processIndex);
}

void Kernel::enqueue(std::uint32_t processIndex) {
    if (designImage->compiledDesign().body(processes[processIndex].bodyIndex).connectsPort) {
        connectionsDue.push_back(processIndex);
        return;
    }
    scheduler.schedule(resumeRegion(processIndex), processIndex);
}

void Kernel::runConnections() {
    Interpreter interpreter(*this);
    while (!connectionsDue.empty()) {
        // Only what the connections drive is applied now; what the cycle's processes have
        // assigned so far stays for the next cycle.
        const std::size_t first = deltaTransactions.size();
        for (const std::uint32_t processIndex : std::exchange(connectionsDue, {})) {
            processes[processIndex].queued = false;
            runningProcess = processIndex;
            const ExecutionResult result = interpreter.run(processIndex);
            runningProcess = NoProcess;
            if (result == ExecutionResult::Stopped || stopping()) return;
        }
        applyDeltaTransactions(first);
        if (stopping()) return;
    }
}

Region Kernel::resumeRegion(std::uint32_t processIndex) const {
    return designImage->compiledDesign().body(processes[processIndex].bodyIndex).postponed
               ? Region::Postponed
               : Region::Active;
}

void Kernel::writeSignal(std::uint32_t signalIndex, eir::ConstBitRef value) {
    if (isOverridden(signalIndex)) return;
    changeSignal(signalIndex, value);
}

void Kernel::changeSignal(std::uint32_t signalIndex, eir::ConstBitRef value) {
    const Signal& signal = designImage->signals()[signalIndex];
    const eir::BitRef target = stateArena.at(signal.wordOffset, signal.bitWidth);

    // Value-change filtering before scheduling, one of the largest single performance
    // wins available: a process is woken when a signal it watches actually changes, not
    // merely when it is assigned.
    if (value.width() == target.width() && eir::ConstBitRef(target).identicalTo(value)) return;

    recordEvent(signalIndex, target);
    const eir::ConstBitRef previous = keepPrevious(signal, target);
    const eir::Logic before = target.get(0);
    bitops::extend(target, value, false);
    const eir::Logic after = target.get(0);

    for (ValueChangeListener* listener : listeners) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after, previous, target);

    // Last, because an application's callback may write this signal again. Everything about
    // this change — the edge it was, who it woke, what the listeners recorded — is settled
    // before that can happen, so a second write is a second change rather than one that
    // rewrites the first behind its back.
    if (pliHooks != nullptr) pliHooks->valueChanged(signalIndex);
}

void Kernel::recordEvent(std::uint32_t signalIndex, eir::ConstBitRef current) {
    if (lastChange[signalIndex] == cycle) return;
    lastChange[signalIndex] = cycle;
    lastActive[signalIndex] = cycle;
    if (const std::uint32_t offset = lastValueOffset[signalIndex]; offset != NoLastValue)
        bitops::copy(eir::BitRef(lastValues.data() + offset, current.width()), current);
}

eir::ConstBitRef Kernel::lastValue(std::uint32_t signalIndex) const {
    const Signal& signal = designImage->signals()[signalIndex];
    const std::uint32_t offset = lastValueOffset[signalIndex];
    if (offset == NoLastValue) return stateArena.at(signal.wordOffset, signal.bitWidth);
    return {lastValues.data() + offset, signal.bitWidth};
}

void Kernel::writeSignalPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                             std::uint64_t lowBit) {
    if (isOverridden(signalIndex)) return;

    const Signal& signal = designImage->signals()[signalIndex];
    const eir::BitRef target = stateArena.at(signal.wordOffset, signal.bitWidth);

    // Change detection over the written window only, compared against the incoming value in
    // place so that no temporary copy of the old value is needed on the hot path.
    if (bitops::holdsPartAt(target, value, lowBit)) return;

    recordEvent(signalIndex, target);
    const eir::ConstBitRef previous = keepPrevious(signal, target);
    const eir::Logic before = target.get(0);
    bitops::assignPartAt(target, value, lowBit);
    const eir::Logic after = target.get(0);

    for (ValueChangeListener* listener : listeners) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after, previous, target);

    if (pliHooks != nullptr) pliHooks->valueChanged(signalIndex);
}

void Kernel::writeThroughDrivers(const WriteRoute& route, eir::ConstBitRef value,
                                 std::uint64_t lowBit) {
    const std::span<const elab::Driver> drivers = designImage->elaboratedDesign().drivers();

    for (std::uint32_t index = route.firstDriver; index < route.firstDriver + route.driverCount;
         ++index) {
        const elab::Driver& driver = drivers[index];
        if (lowBit < driver.lowBit || lowBit >= driver.endBit()) continue;

        const eir::BitRef storage = stateArena.at(driver.wordOffset, driver.width);
        const std::uint64_t offset = lowBit - driver.lowBit;

        // A driver that did not change changes nothing it resolves into, so the common case
        // of a continuous assignment re-evaluating to the value it already drives stops here.
        if (bitops::holdsPartAt(storage, value, offset)) return;

        bitops::assignPartAt(storage, value, offset);
        resolveDriver(index);
        return;
    }
}

void Kernel::resolveDriver(std::uint32_t driverIndex) {
    const elab::ElaboratedDesign& elaborated = designImage->elaboratedDesign();
    const elab::Driver& written = elaborated.drivers()[driverIndex];
    const eir::BitRef resolved(resolvedScratch.data(), written.width);

    for (std::uint32_t segmentIndex = written.firstSegment;
         segmentIndex < written.firstSegment + written.segmentCount; ++segmentIndex) {
        const elab::Segment& segment = elaborated.segments()[segmentIndex];
        const eir::BitRef acc(segmentScratch.data(), segment.width);
        if (!resolveSegment(written.process, segment, acc)) return;
        bitops::assignPartAt(resolved, acc, segment.lowBit - written.lowBit);
    }

    writeSignalPart(written.slot, resolved, written.lowBit);
}

bool Kernel::resolveSegment(std::uint32_t processIndex, const elab::Segment& segment,
                            eir::BitRef acc) {
    const elab::ElaboratedDesign& elaborated = designImage->elaboratedDesign();
    const eir::BitRef next(driverScratch.data(), segment.width);

    const auto sliceOf = [&](std::uint32_t segmentDriver, eir::BitRef into) {
        const elab::Driver& driver =
            elaborated.drivers()[elaborated.segmentDrivers()[segmentDriver]];
        bitops::partSelectAt(into, stateArena.at(driver.wordOffset, driver.width),
                             segment.lowBit - driver.lowBit);
    };

    // A VHDL resolved signal's function, called for each subelement; a net's table.
    if (segment.resolution != elab::Segment::NoResolution)
        return resolveByFunction(processIndex, segment, acc);

    // Each driver folded in by the net's table.
    const auto fold = [&segment](eir::BitRef into, eir::ConstBitRef driver) {
        switch (segment.netKind) {
        case eir::NetKind::WiredAnd:
        case eir::NetKind::TriAnd: bitops::resolveWiredAnd(into, driver); break;
        case eir::NetKind::WiredOr:
        case eir::NetKind::TriOr: bitops::resolveWiredOr(into, driver); break;
        case eir::NetKind::Wire:
        case eir::NetKind::Tri:
        case eir::NetKind::Tri0:
        case eir::NetKind::Tri1:
        case eir::NetKind::Supply0:
        case eir::NetKind::Supply1:
        case eir::NetKind::UWire: bitops::resolveWire(into, driver); break;
        }
    };

    if (!segment.weak) {
        sliceOf(segment.firstDriver, acc);
        for (std::uint32_t other = segment.firstDriver + 1;
             other < segment.firstDriver + segment.driverCount; ++other) {
            sliceOf(other, next);
            fold(acc, next);
        }
    } else {
        // The strong class decides each bit, and the weak only where the strong leave it
        // `z` (DESIGN_GUIDE.md section 3.7).
        const eir::BitRef weak(weakScratch.data(), segment.width);
        const eir::BitRef part(classScratch.data(), segment.width);
        acc.fill(eir::Logic::HighZ);
        weak.fill(eir::Logic::HighZ);
        for (std::uint32_t index = segment.firstDriver;
             index < segment.firstDriver + segment.driverCount; ++index) {
            const eir::DriveStrength strength =
                elaborated.drivers()[elaborated.segmentDrivers()[index]].strength;
            sliceOf(index, next);
            bitops::keepClass(part, next, !eir::isWeak(strength.zero), !eir::isWeak(strength.one));
            fold(acc, part);
            bitops::keepClass(part, next, eir::isWeak(strength.zero), eir::isWeak(strength.one));
            fold(weak, part);
        }
        bitops::fillHighImpedance(acc, weak);
    }

    // What the net itself contributes: a pull where every driver is `z`, or a supply that
    // no strong driver can move.
    switch (segment.netKind) {
    case eir::NetKind::Tri0: bitops::pullHighImpedance(acc, eir::Logic::Zero); break;
    case eir::NetKind::Tri1: bitops::pullHighImpedance(acc, eir::Logic::One); break;
    case eir::NetKind::Supply0: acc.fill(eir::Logic::Zero); break;
    case eir::NetKind::Supply1: acc.fill(eir::Logic::One); break;
    case eir::NetKind::Wire:
    case eir::NetKind::Tri:
    case eir::NetKind::WiredAnd:
    case eir::NetKind::WiredOr:
    case eir::NetKind::TriAnd:
    case eir::NetKind::TriOr:
    case eir::NetKind::UWire: break;
    }
    return true;
}

void Kernel::resolveInitialValues() {
    // A VHDL resolved signal starts at what its drivers' initial values resolve to (IEEE 1076
    // section 14.7.5.2), written as the value it has rather than as a change of it.
    const elab::ElaboratedDesign& elaborated = designImage->elaboratedDesign();
    for (const elab::Segment& segment : elaborated.segments()) {
        const elab::Slot& slot = elaborated.slots()[segment.slot];
        if (!slot.resolution.resolved() || segment.driverCount == 0) continue;
        const elab::Driver& first =
            elaborated.drivers()[elaborated.segmentDrivers()[segment.firstDriver]];
        const eir::BitRef acc(segmentScratch.data(), segment.width);
        // A lone driver whose function is proven to give back what it is given is its value.
        if (segment.resolution == elab::Segment::NoResolution) {
            bitops::partSelectAt(acc, stateArena.at(first.wordOffset, first.width),
                                 segment.lowBit - first.lowBit);
        } else if (!resolveByFunction(first.process, segment, acc)) {
            return;
        }
        bitops::assignPartAt(stateArena.at(slot.wordOffset, slot.type.bitWidth()), acc,
                             segment.lowBit);
    }
}

bool Kernel::resolveByFunction(std::uint32_t processIndex, const elab::Segment& segment,
                               eir::BitRef acc) {
    const elab::ElaboratedDesign& elaborated = designImage->elaboratedDesign();
    const elab::ResolutionCall& call = elaborated.resolutionCalls()[segment.resolution];
    const std::uint32_t elementBits = call.elementBits;
    const std::uint32_t count = segment.driverCount;
    eir::BitVector drivers = eir::BitVector::filled(elementBits * count, eir::Logic::Zero);
    eir::BitVector one(elementBits);
    for (std::uint32_t offset = 0; offset + elementBits <= segment.width; offset += elementBits) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const elab::Driver& driver =
                elaborated.drivers()[elaborated.segmentDrivers()[segment.firstDriver + index]];
            bitops::partSelectAt(one.mutableRef(), stateArena.at(driver.wordOffset, driver.width),
                                 segment.lowBit + offset - driver.lowBit);
            bitops::assignPartAt(drivers.mutableRef(), one.ref(),
                                 static_cast<std::uint64_t>(count - 1 - index) * elementBits);
        }
        if (!callResolution(processIndex, call.subprogramInstance, drivers.ref(), one.mutableRef()))
            return false;
        bitops::assignPartAt(acc, one.ref(), offset);
    }
    return true;
}

bool Kernel::callResolution(std::uint32_t processIndex, std::uint32_t subprogram,
                            eir::ConstBitRef drivers, eir::BitRef resolved) {
    const BoundSubprogram& callee = designImage->subprograms()[subprogram];
    const codegen::CompiledBody& body = designImage->compiledDesign().body(callee.bodyIndex);

    // A frame above whatever the call stack holds, its registers as any call's start.
    const std::uint32_t frame = callStackTop;
    callStackTop += body.frameWords;
    ENZAN_ASSERT(callStackTop <= frameStorage.size(),
                 "the image sizes the call stack for every resolution function's chain");
    startFrame(frame, body);
    const codegen::CompiledBody::Parameter& parameter = body.parameters.front();
    bitops::copy(frameAt(frame + parameter.offset, parameter.bitWidth), drivers);

    Interpreter interpreter(*this);
    const ExecutionResult outcome =
        interpreter.call(processIndex, Activation{.body = &body,
                                                  .frameOffset = frame,
                                                  .objectSignals = callee.objectSignals,
                                                  .writeSignals = callee.writeSignals,
                                                  .writeRoutes = callee.writeRoutes,
                                                  .calls = callee.calls});
    const bool returned = outcome == ExecutionResult::Completed;
    if (returned) bitops::copy(resolved, frameAt(frame + body.resultOffset, body.resultWidth));
    callStackTop = frame;
    return returned;
}

void Kernel::scheduleNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value) {
    scheduler.appendNonBlocking(signalIndex, value, 0, false);
}

void Kernel::scheduleNonBlockingPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                                     std::uint64_t lowBit) {
    scheduler.appendNonBlocking(signalIndex, value, static_cast<std::uint32_t>(lowBit), true);
}

bool Kernel::isForced(std::uint32_t signalIndex) const noexcept {
    return !overrides.empty() && overrides[signalIndex].force != NoHolder;
}

bool Kernel::isOverridden(std::uint32_t signalIndex) const noexcept {
    return !overrides.empty() && overrides[signalIndex].held();
}

void Kernel::deposit(std::uint32_t signalIndex, eir::ConstBitRef value) {
    writeSignal(signalIndex, value);
}

void Kernel::force(std::uint32_t signalIndex, eir::ConstBitRef value) {
    if (overrides.empty()) overrides.resize(designImage->signals().size());
    overrides[signalIndex].force = FromOutside;
    changeSignal(signalIndex, value);
}

bool Kernel::release(std::uint32_t signalIndex) {
    if (!isForced(signalIndex)) return false;
    Overrides& held = overrides[signalIndex];
    held.force = NoHolder;

    // An assign the force stood over is re-established and evaluated again; anything else
    // goes back to what drives it (IEEE 1800 section 10.6.2).
    if (held.assign != NoHolder) {
        wake(held.assign);
        return true;
    }
    restoreDriven(signalIndex);
    return true;
}

void Kernel::startOverride(std::uint32_t processIndex, std::uint32_t holder,
                           std::uint32_t signalIndex, eir::ConstBitRef value, bool force) {
    // The holder is a process of the same instance, which numbers its processes from the
    // definition's first.
    const std::uint32_t first =
        processIndex - designImage->elaboratedDesign().processes()[processIndex].processIndex;
    if (overrides.empty()) overrides.resize(designImage->signals().size());
    Overrides& held = overrides[signalIndex];
    if (force) {
        held.force = first + holder;
    } else {
        held.assign = first + holder;
        if (held.force != NoHolder) return;
    }
    changeSignal(signalIndex, value);
}

void Kernel::deassign(std::uint32_t signalIndex) {
    if (!overrides.empty()) overrides[signalIndex].assign = NoHolder;
}

void Kernel::reassert(std::uint32_t processIndex, std::uint32_t signalIndex,
                      eir::ConstBitRef value) {
    if (overrides.empty()) return;
    const Overrides& held = overrides[signalIndex];
    if (held.force == processIndex || (held.force == NoHolder && held.assign == processIndex))
        changeSignal(signalIndex, value);
}

void Kernel::restoreDriven(std::uint32_t signalIndex) {
    const elab::ElaboratedDesign& elaborated = designImage->elaboratedDesign();
    const elab::Slot& slot = elaborated.slots()[signalIndex];
    const Signal& signal = designImage->signals()[signalIndex];

    // Segments are made slot by slot, so a slot's are one run of them.
    const auto segments =
        std::ranges::equal_range(elaborated.segments(), signalIndex, {}, &elab::Segment::slot);
    if (slot.kind != eir::ObjectKind::Net && segments.empty()) return;

    eir::BitVector value = eir::BitVector::from(stateArena.at(signal.wordOffset, signal.bitWidth));
    if (slot.kind == eir::ObjectKind::Net) {
        const eir::Logic undriven = eir::undrivenValue(slot.netKind);
        std::uint32_t bit = 0;
        const auto fillTo = [&](std::uint32_t end) {
            if (end > bit)
                bitops::assignPartAt(value.mutableRef(),
                                     eir::BitVector::filled(end - bit, undriven).ref(), bit);
        };
        for (const eir::WriteExtent& driven : slot.drivenBits) {
            fillTo(driven.lowBit);
            bit = std::max(bit, driven.endBit());
        }
        fillTo(signal.bitWidth);
    }

    for (const elab::Segment& segment : segments) {
        const elab::Driver& first =
            elaborated.drivers()[elaborated.segmentDrivers()[segment.firstDriver]];
        const eir::BitRef acc(segmentScratch.data(), segment.width);
        if (!resolveSegment(first.process, segment, acc)) return;
        bitops::assignPartAt(value.mutableRef(), acc, segment.lowBit);
    }
    writeSignal(signalIndex, value.ref());
}

void Kernel::addListener(ValueChangeListener& newListener) {
    listeners.push_back(&newListener);
}

void Kernel::removeListener(ValueChangeListener& listener) {
    const auto position = std::ranges::find(listeners, &listener);
    if (position != listeners.end()) listeners.erase(position);
}

eir::ConstBitRef Kernel::keepPrevious(const Signal& signal, eir::ConstBitRef target) {
    if (!signal.windowed) return {};
    const eir::BitRef kept(windowScratch.data(), signal.bitWidth);
    bitops::copy(kept, target);
    return kept;
}

void Kernel::notifyChange(std::uint32_t signalIndex, eir::Logic before, eir::Logic after,
                          eir::ConstBitRef previous, eir::ConstBitRef current) {
    // A whole-signal entry looks at the lowest bit for an edge and needs nothing for a change;
    // a window looks at its own lowest bit, or compares its bits, against the kept old value.
    const auto matches = [&](eir::EdgeKind edge, std::uint32_t lowBit, std::uint32_t width) {
        if (width == 0) return edgeMatches(edge, before, after);
        if (edge != eir::EdgeKind::AnyChange)
            return edgeMatches(edge, previous.get(lowBit), current.get(lowBit));
        return !bitops::samePartAt(previous, current, lowBit, width);
    };

    for (const Signal::Subscriber& subscriber : designImage->signals()[signalIndex].subscribers) {
        if (matches(subscriber.edge, subscriber.lowBit, subscriber.width))
            wake(subscriber.processIndex);
    }

    // Dynamic subscriptions are consumed when they fire: a process that waited for one
    // edge is not waiting for the next until it asks again. Rewritten in place rather
    // than erased one by one, so that the common case of nothing matching costs one pass. One
    // armed by a wait another entry of which has already fired is stale, and goes.
    std::vector<EdgeWaiter>& waiters = edgeWaiters[signalIndex].waiters;
    std::size_t kept = 0;
    for (std::size_t index = 0; index < waiters.size(); ++index) {
        const EdgeWaiter waiter = waiters[index];
        ProcessState& state = processes[waiter.processIndex];
        if (waiter.generation != state.waitGeneration) continue;
        if (matches(waiter.edge, waiter.lowBit, waiter.width)) {
            ++state.waitGeneration;
            cancelTimeout(waiter.processIndex);
            resume(waiter.processIndex, waiter.resumePc);
        } else {
            waiters[kept++] = waiter;
        }
    }
    waiters.resize(kept);
}

void Kernel::suspendForTime(std::uint32_t processIndex, SimTime delay, std::uint32_t resumePc) {
    ProcessState& state = processes[processIndex];
    state.resumePc = resumePc;

    if (delay == 0) {
        // VHDL's resumes in the next delta cycle, after its transactions are applied.
        if (designImage->compiledDesign().body(state.bodyIndex).resumesNextCycle) {
            nextCycleResumes.push_back(Timeout{.processIndex = processIndex,
                                               .generation = state.waitGeneration,
                                               .resumePc = resumePc});
            return;
        }
        // `#0` resumes in this timestamp but after everything currently Active, which is
        // exactly what the Inactive region is for.
        state.queued = true;
        scheduler.schedule(Region::Inactive, processIndex);
        return;
    }

    if (delay > NoTime - currentTime) {
        diagnostics.report(DiagCode::TimeOverflow, SourceLocation(), currentTime,
                           options.timeScale.unitName());
        errorRequested = true;
        return;
    }

    state.queued = true;
    scheduler.scheduleAt(currentTime + delay, processIndex, resumeRegion(processIndex));
}

void Kernel::scheduleDelayedWrite(std::uint32_t processIndex, std::uint32_t position,
                                  std::uint32_t signalIndex, eir::ConstBitRef value, SimTime delay,
                                  DelayKind kind, std::optional<std::uint32_t> lowBit) {
    const bool nonBlocking = kind == DelayKind::NonBlocking;
    if (delay > NoTime - currentTime) {
        diagnostics.report(DiagCode::TimeOverflow, SourceLocation(), currentTime,
                           options.timeScale.unitName());
        errorRequested = true;
        return;
    }

    if (nonBlocking && delay == 0) {
        scheduler.appendNonBlocking(signalIndex, value, lowBit.value_or(0), lowBit.has_value());
        return;
    }

    DelayedWrite write{.processIndex = processIndex,
                       .position = position,
                       .signalIndex = signalIndex,
                       .nonBlocking = nonBlocking,
                       .serial = ++nextWriteSerial,
                       .value = eir::BitVector::from(value),
                       .lowBit = lowBit};

    if (kind == DelayKind::Transport && delay == 0) {
        writeRouted(processIndex, position, signalIndex, value, lowBit);
        return;
    }
    if (kind == DelayKind::Inertial) {
        // The write this one replaces is taken out rather than skipped when it comes due, so
        // that it leaves no time step behind in which nothing happens.
        const auto key = std::tuple{processIndex, position, lowBit.value_or(WholeSignal)};
        if (const auto pending = pendingInertial.find(key); pending != pendingInertial.end()) {
            const auto slot = delayedWrites.find(pending->second.time);
            std::erase_if(slot->second, [&pending](const DelayedWrite& queued) {
                return queued.serial == pending->second.serial;
            });
            if (slot->second.empty()) delayedWrites.erase(slot);
            pendingInertial.erase(pending);
        }
        if (delay == 0) {
            writeRouted(processIndex, position, signalIndex, value, lowBit);
            return;
        }
        pendingInertial.emplace(
            key, PendingInertial{.serial = write.serial, .time = currentTime + delay});
    }

    delayedWrites[currentTime + delay].push_back(std::move(write));
}

void Kernel::applyDelayedWrites() {
    const auto due = delayedWrites.find(currentTime);
    if (due == delayedWrites.end()) return;
    const std::vector<DelayedWrite> writes = std::move(due->second);
    delayedWrites.erase(due);

    for (const DelayedWrite& write : writes) {
        if (write.transaction) {
            const std::optional<Transaction> taken =
                takeTransaction(write.processIndex, write.position, write.serial);
            if (taken)
                applyTransaction(write.processIndex, write.position, write.signalIndex, *taken);
            continue;
        }
        if (write.nonBlocking) {
            scheduler.appendNonBlocking(write.signalIndex, write.value.ref(),
                                        write.lowBit.value_or(0), write.lowBit.has_value());
            continue;
        }
        pendingInertial.erase(
            std::tuple{write.processIndex, write.position, write.lowBit.value_or(WholeSignal)});
        writeRouted(write.processIndex, write.position, write.signalIndex, write.value.ref(),
                    write.lowBit);
    }
}

void Kernel::drive(std::uint32_t processIndex, std::uint32_t position, std::uint32_t signalIndex,
                   eir::ConstBitRef value, std::uint32_t lowBit, SimTime delay, bool inertial) {
    if (delay > NoTime - currentTime) {
        diagnostics.report(DiagCode::TimeOverflow, SourceLocation(), currentTime,
                           options.timeScale.unitName());
        errorRequested = true;
        return;
    }
    const SimTime time = currentTime + delay;
    std::vector<Transaction>& waveform = projectedWaveforms[driverBase[processIndex] + position];
    const std::uint32_t end = lowBit + value.width();
    const auto overlaps = [lowBit, end](const Transaction& transaction) {
        return transaction.lowBit < end && lowBit < transaction.endBit();
    };

    // The bits it assigns lose every transaction at or after its time (IEEE 1076 section
    // 10.5.2.2), so of a process's assignments to a signal in one run, the last is what happens.
    // Each scalar subelement has a driver of its own, so a transaction reaching over bits this
    // one does not assign keeps them, cut down to what remains.
    std::vector<Transaction> kept;
    kept.reserve(waveform.size() + 1);
    for (Transaction& old : waveform) {
        if (old.time < time || !overlaps(old)) {
            kept.push_back(std::move(old));
            continue;
        }
        unscheduleTransaction(old);
        const DriverOf driver{
            .processIndex = processIndex, .position = position, .signalIndex = signalIndex};
        if (old.lowBit < lowBit) kept.push_back(remainder(driver, old, old.lowBit, lowBit));
        if (end < old.endBit()) kept.push_back(remainder(driver, old, end, old.endBit()));
    }
    waveform = std::move(kept);

    eir::BitVector stored = eir::BitVector::from(value);

    // Inertially, so do those within the rejection limit before it, which is the delay, so every
    // one still pending; but for a run of transactions of the new value immediately before it,
    // which the new one only prolongs.
    if (inertial && delay > 0) {
        std::vector<bool> deleted(waveform.size(), false);
        bool run = true;
        for (std::size_t index = waveform.size(); index-- > 0;) {
            const Transaction& old = waveform[index];
            if (!overlaps(old)) continue;
            if (old.lowBit != lowBit || old.value.width() != value.width()) {
                // Rejection is decided for each subelement, and this transaction covers some of
                // the new one's and not others; deciding it for the whole would be wrong.
                diagnostics.report(DiagCode::NotYetImplemented, SourceLocation(),
                                   "an inertial assignment to a signal whose driver has a "
                                   "transaction pending for only part of what it assigns");
                errorRequested = true;
                return;
            }
            if (run && old.value == stored) continue;
            run = false;
            deleted[index] = true;
            unscheduleTransaction(old);
        }
        std::size_t next = 0;
        std::erase_if(waveform, [&deleted, &next](const Transaction&) { return deleted[next++]; });
    }

    // After every transaction of its time or earlier, so the waveform stays in time order.
    const std::uint64_t serial = ++nextWriteSerial;
    schedule({.processIndex = processIndex, .position = position, .signalIndex = signalIndex}, time,
             serial);
    const auto place = std::ranges::upper_bound(waveform, time, {}, &Transaction::time);
    waveform.insert(
        place,
        Transaction{.time = time, .serial = serial, .lowBit = lowBit, .value = std::move(stored)});
}

void Kernel::schedule(const DriverOf& driver, SimTime time, std::uint64_t serial) {
    if (time == currentTime) {
        deltaTransactions.push_back(DeltaTransaction{.processIndex = driver.processIndex,
                                                     .position = driver.position,
                                                     .signalIndex = driver.signalIndex,
                                                     .serial = serial});
        return;
    }
    // The value stays on the waveform, where a later assignment compares against it.
    delayedWrites[time].push_back(DelayedWrite{.processIndex = driver.processIndex,
                                               .position = driver.position,
                                               .signalIndex = driver.signalIndex,
                                               .nonBlocking = false,
                                               .transaction = true,
                                               .serial = serial,
                                               .value = eir::BitVector(1)});
}

Kernel::Transaction Kernel::remainder(const DriverOf& driver, const Transaction& transaction,
                                      std::uint32_t lowBit, std::uint32_t endBit) {
    eir::BitVector part(endBit - lowBit);
    eir::bitops::partSelectAt(part.mutableRef(), transaction.value.ref(),
                              lowBit - transaction.lowBit);
    const std::uint64_t serial = ++nextWriteSerial;
    schedule(driver, transaction.time, serial);
    return Transaction{
        .time = transaction.time, .serial = serial, .lowBit = lowBit, .value = std::move(part)};
}

void Kernel::unscheduleTransaction(const Transaction& transaction) {
    // One for now waits in `deltaTransactions`, which skips it once it is off its waveform.
    if (transaction.time == currentTime) return;
    const auto slot = delayedWrites.find(transaction.time);
    if (slot == delayedWrites.end()) return;
    std::erase_if(slot->second, [&transaction](const DelayedWrite& queued) {
        return queued.transaction && queued.serial == transaction.serial;
    });
    if (slot->second.empty()) delayedWrites.erase(slot);
}

std::optional<Kernel::Transaction>
Kernel::takeTransaction(std::uint32_t processIndex, std::uint32_t position, std::uint64_t serial) {
    std::vector<Transaction>& waveform = projectedWaveforms[driverBase[processIndex] + position];
    const auto found = std::ranges::find(waveform, serial, &Transaction::serial);
    if (found == waveform.end()) return std::nullopt;
    Transaction taken = std::move(*found);
    waveform.erase(found);
    return taken;
}

void Kernel::applyTransaction(std::uint32_t processIndex, std::uint32_t position,
                              std::uint32_t signalIndex, const Transaction& transaction) {
    // A transaction makes its signal active, whether or not it changes it (IEEE 1076 section
    // 14.7.3.1).
    lastActive[signalIndex] = cycle;
    const WriteRoute& route = processes[processIndex].writeRoutes[position];
    if (!route.direct()) {
        writeThroughDrivers(route, transaction.value.ref(), transaction.lowBit);
    } else if (transaction.lowBit == 0
               && transaction.value.width() == designImage->signals()[signalIndex].bitWidth) {
        writeSignal(signalIndex, transaction.value.ref());
    } else {
        writeSignalPart(signalIndex, transaction.value.ref(), transaction.lowBit);
    }
}

void Kernel::applyDeltaTransactions(std::size_t first) {
    // Taken as a batch, as non-blocking updates are: a value-change callback may assign, and
    // what it assigns belongs to the delta cycle after this one.
    const auto from = deltaTransactions.begin() + static_cast<std::ptrdiff_t>(first);
    const std::vector<DeltaTransaction> due(from, deltaTransactions.end());
    deltaTransactions.erase(from, deltaTransactions.end());
    for (const DeltaTransaction& transaction : due) {
        const std::optional<Transaction> taken =
            takeTransaction(transaction.processIndex, transaction.position, transaction.serial);
        if (!taken) continue;
        applyTransaction(transaction.processIndex, transaction.position, transaction.signalIndex,
                         *taken);
        if (stopping()) break;
    }
}

void Kernel::writeRouted(std::uint32_t processIndex, std::uint32_t position,
                         std::uint32_t signalIndex, eir::ConstBitRef value,
                         std::optional<std::uint32_t> lowBit) {
    const WriteRoute& route = processes[processIndex].writeRoutes[position];
    if (!route.direct()) {
        writeThroughDrivers(route, value, lowBit.value_or(0));
    } else if (lowBit.has_value()) {
        writeSignalPart(signalIndex, value, *lowBit);
    } else {
        writeSignal(signalIndex, value);
    }
}

void Kernel::suspendForEdge(std::uint32_t processIndex, std::uint32_t signalIndex,
                            eir::EdgeKind edge, std::uint32_t resumePc) {
    ProcessState& state = processes[processIndex];
    state.resumePc = resumePc;
    armWaiter(signalIndex, EdgeWaiter{.processIndex = processIndex,
                                      .edge = edge,
                                      .resumePc = resumePc,
                                      .generation = state.waitGeneration});
}

void Kernel::suspendForEvents(std::uint32_t processIndex, std::uint32_t listIndex,
                              std::uint32_t resumePc) {
    ProcessState& state = processes[processIndex];
    state.resumePc = resumePc;
    const codegen::CompiledBody& body = designImage->compiledDesign().body(state.bodyIndex);
    for (const eir::SensitivityEntry& entry : body.eventLists[listIndex]) {
        armWaiter(state.objectSignals[entry.objectIndex],
                  EdgeWaiter{.processIndex = processIndex,
                             .edge = entry.edge,
                             .resumePc = resumePc,
                             .generation = state.waitGeneration,
                             .lowBit = entry.lowBit,
                             .width = entry.width});
    }
}

void Kernel::suspendForEventsFor(std::uint32_t processIndex, std::uint32_t listIndex,
                                 std::uint32_t resumePc, SimTime delay) {
    suspendForEvents(processIndex, listIndex, resumePc);
    const Timeout timeout{.processIndex = processIndex,
                          .generation = processes[processIndex].waitGeneration,
                          .resumePc = resumePc};
    if (delay == 0) {
        nextCycleResumes.push_back(timeout);
        return;
    }
    if (delay > NoTime - currentTime) {
        diagnostics.report(DiagCode::TimeOverflow, SourceLocation(), currentTime,
                           options.timeScale.unitName());
        errorRequested = true;
        return;
    }
    timeouts[currentTime + delay].push_back(timeout);
    timeoutOf[processIndex] = currentTime + delay;
}

void Kernel::resumeTimedOut(const Timeout& timeout) {
    ProcessState& state = processes[timeout.processIndex];
    if (timeout.generation != state.waitGeneration) return;
    ++state.waitGeneration;
    resume(timeout.processIndex, timeout.resumePc);
}

void Kernel::cancelTimeout(std::uint32_t processIndex) {
    const SimTime time = std::exchange(timeoutOf[processIndex], NoTime);
    if (time == NoTime) return;
    const auto due = timeouts.find(time);
    if (due == timeouts.end()) return;
    const auto ofProcess = [processIndex](const Timeout& each) {
        return each.processIndex == processIndex;
    };
    std::erase_if(due->second, ofProcess);
    if (due->second.empty()) timeouts.erase(due);
}

void Kernel::armWaiter(std::uint32_t signalIndex, const EdgeWaiter& waiter) {
    EdgeWaiterList& list = edgeWaiters[signalIndex];
    if (list.waiters.size() >= list.sweepAt) {
        std::erase_if(list.waiters, [this](const EdgeWaiter& armed) {
            return armed.generation != processes[armed.processIndex].waitGeneration;
        });
        list.sweepAt = std::max(MinimumWaiterSweep, 2 * list.waiters.size());
    }
    list.waiters.push_back(waiter);
}

std::optional<Activation> Kernel::enterCall(std::uint32_t processIndex, const Activation& caller,
                                            std::uint32_t callIndex, std::uint32_t resumePc) {
    const codegen::CallDescriptor& call = caller.body->calls[callIndex];
    const BoundSubprogram& callee = designImage->subprograms()[caller.calls[callIndex]];
    const codegen::CompiledBody& body = designImage->compiledDesign().body(callee.bodyIndex);

    if (callsInProgress.size() >= callDepthCapacity) {
        diagnostics.report(DiagCode::CallDepthExceeded, call.location, options.callDepthLimit,
                           body.name, processes[processIndex].name,
                           options.timeScale.format(currentTime));
        errorRequested = true;
        unwindCalls();
        return std::nullopt;
    }

    // The callee's registers start at zero on every call, so that nothing of an earlier call's
    // can be read by this one.
    const std::uint32_t frame = callStackTop;
    callStackTop += body.frameWords;
    startFrame(frame, body);

    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        const codegen::CallDescriptor::Argument& argument = call.arguments[index];
        const codegen::CompiledBody::Parameter& parameter = body.parameters[index];
        // An `out` parameter starts as the callee's own, not as the caller's value.
        if (parameter.mode == eir::ParameterMode::Out) continue;
        const eir::ConstBitRef value =
            argument.isConstant
                ? caller.body->constant(argument.location)
                : frameAt(caller.frameOffset + argument.location, argument.bitWidth);
        eir::bitops::copy(frameAt(frame + parameter.offset, parameter.bitWidth), value);
    }

    callsInProgress.push_back(
        CallInProgress{.caller = caller, .callIndex = callIndex, .resumePc = resumePc});
    return Activation{.body = &body,
                      .frameOffset = frame,
                      .objectSignals = callee.objectSignals,
                      .writeSignals = callee.writeSignals,
                      .writeRoutes = callee.writeRoutes,
                      .calls = callee.calls};
}

std::optional<std::pair<Activation, std::uint32_t>> Kernel::leaveCall(const Activation& callee) {
    if (callsInProgress.empty()) return std::nullopt;

    const CallInProgress returning = callsInProgress.back();
    callsInProgress.pop_back();
    const Activation& caller = returning.caller;
    const codegen::CallDescriptor& call = caller.body->calls[returning.callIndex];
    const codegen::CompiledBody& body = *callee.body;

    if (call.hasResult) {
        eir::bitops::copy(frameAt(caller.frameOffset + call.resultLocation, call.resultWidth),
                          frameAt(callee.frameOffset + body.resultOffset, body.resultWidth));
    }
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        const codegen::CompiledBody::Parameter& parameter = body.parameters[index];
        if (parameter.mode == eir::ParameterMode::In) continue;
        const codegen::CallDescriptor::Argument& argument = call.arguments[index];
        eir::bitops::copy(frameAt(caller.frameOffset + argument.location, argument.bitWidth),
                          frameAt(callee.frameOffset + parameter.offset, parameter.bitWidth));
    }

    callStackTop = callee.frameOffset;
    return std::pair{caller, returning.resumePc};
}

void Kernel::unwindCalls() noexcept {
    callsInProgress.clear();
    callStackTop = callStackBase;
}

void Kernel::runSystemTask(std::uint32_t processIndex, const Activation& activation,
                           std::uint32_t sysCallIndex) {
    const ProcessState& state = processes[processIndex];
    const codegen::CompiledBody& body = *activation.body;
    const codegen::SysTaskDescriptor& descriptor = body.sysCalls[sysCallIndex];

    std::vector<FormatArgument> arguments;
    arguments.reserve(descriptor.arguments.size());
    for (const codegen::SysTaskDescriptor::Argument& argument : descriptor.arguments) {
        const eir::ConstBitRef value =
            argument.isConstant
                ? body.constant(argument.location)
                : frameAt(activation.frameOffset + argument.location, argument.bitWidth);
        arguments.push_back(
            FormatArgument{.value = value,
                           .isSigned = argument.isSigned,
                           .isReal = argument.isReal,
                           .text = argument.isString ? &scratchAt(value.aval()[0]).text : nullptr});
    }

    // The file tasks take a descriptor first, which is not part of what they print. An
    // unknown bit in it names no file.
    const bool toFile = descriptor.kind == eir::SysTaskKind::FileDisplay
                        || descriptor.kind == eir::SysTaskKind::FileWrite
                        || descriptor.kind == eir::SysTaskKind::FileClose
                        || descriptor.kind == eir::SysTaskKind::FileFlush;
    std::uint32_t fileDescriptor = 0;
    if (toFile && !arguments.empty()) {
        const eir::ConstBitRef value = arguments.front().value;
        fileDescriptor = static_cast<std::uint32_t>(value.aval()[0] & ~value.bval()[0]);
        arguments.erase(arguments.begin());
    }

    // A VHDL report's severity comes first too, and is not part of its message either.
    std::uint64_t severity = 0;
    if (descriptor.kind == eir::SysTaskKind::Report && !arguments.empty()) {
        severity = arguments.front().value.toUInt64();
        arguments.erase(arguments.begin());
    }

    const elab::Instance& instance =
        designImage->elaboratedDesign().instances()[state.instanceIndex];
    const std::string scope = elab::dottedPath(instance.path);
    const FormatContext context{.time = currentTime,
                                .timeScale = options.timeScale,
                                .argumentUnit = argumentUnit(descriptor),
                                .scopeName = scope};

    const std::string text =
        descriptor.hasFormat()
            ? formatSystemTask(body.strings[descriptor.formatIndex], arguments, context)
            : formatDefault(arguments);

    switch (descriptor.kind) {
    case eir::SysTaskKind::Display: transcriptStream() << text << '\n'; return;

    case eir::SysTaskKind::Write: transcriptStream() << text; return;

    case eir::SysTaskKind::Finish: finishRequested = true; return;

    case eir::SysTaskKind::Fatal:
        diagnostics.report(DiagCode::UserFatal, descriptor.location, text);
        finishRequested = true;
        errorRequested = true;
        finishStatus = 1;
        return;

    case eir::SysTaskKind::Error:
        diagnostics.report(DiagCode::UserError, descriptor.location, text);
        return;

    case eir::SysTaskKind::Warning:
        diagnostics.report(DiagCode::UserWarning, descriptor.location, text);
        return;

    case eir::SysTaskKind::Info:
        diagnostics.report(DiagCode::UserInfo, descriptor.location, text);
        return;

    case eir::SysTaskKind::Report: report(severity, descriptor.location, text); return;

    case eir::SysTaskKind::DumpFile:
        for (ValueChangeListener* listener : listeners) listener->onDumpFile(text);
        return;

    case eir::SysTaskKind::DumpVars:
        for (ValueChangeListener* listener : listeners) listener->onDumpVars();
        return;

    case eir::SysTaskKind::FileDisplay:
        writeToDescriptor(fileDescriptor, text + '\n', "$fdisplay", descriptor.location);
        return;

    case eir::SysTaskKind::FileWrite:
        writeToDescriptor(fileDescriptor, text, "$fwrite", descriptor.location);
        return;

    case eir::SysTaskKind::FileClose:
        if (fileFor(fileDescriptor) != nullptr) {
            std::erase_if(openFiles, [fileDescriptor](const OpenFile& file) {
                return file.descriptor == fileDescriptor;
            });
        } else {
            diagnostics.report(DiagCode::FileNotOpen, descriptor.location, "$fclose",
                               fileDescriptor);
        }
        return;

    case eir::SysTaskKind::FileFlush:
        if (descriptor.arguments.empty()) {
            transcriptStream().flush();
            for (OpenFile& file : openFiles) {
                if (file.stream) file.stream->flush();
            }
        } else if (OpenFile* file = fileFor(fileDescriptor); file != nullptr && file->stream) {
            file->stream->flush();
        } else {
            transcriptStream().flush();
        }
        return;

    case eir::SysTaskKind::ReadMemHex:
    case eir::SysTaskKind::ReadMemBinary:
        readMemory(activation.objectSignals, descriptor, arguments);
        return;

    case eir::SysTaskKind::LineDeallocate:
        if (!arguments.empty()) freeLine(arguments.front().value.toUInt64());
        return;

    case eir::SysTaskKind::StringSplice: {
        ScratchString& string = scratchAt(arguments[0].value.toUInt64());
        const auto index = static_cast<std::int64_t>(arguments[1].value.toUInt64());
        const eir::ConstBitRef characters = arguments[2].value;
        const std::uint32_t count = characters.width() / 8;
        if (!withinOrFailure(string, index, count, descriptor.location)) return;
        const auto from = static_cast<std::size_t>(string.position(index));
        for (std::uint32_t at = 0; at < count; ++at)
            string.text[from + at] = characterAt(characters, std::uint64_t{count - 1 - at} * 8);
        return;
    }

    case eir::SysTaskKind::StringRelease: releaseScratch(arguments[0].value.toUInt64()); return;

    case eir::SysTaskKind::FileWriteValue: {
        if (arguments.size() < 2) return;
        const auto file = static_cast<std::uint32_t>(arguments[0].value.toUInt64());
        OpenFile* open = fileFor(file);
        if (open == nullptr || !open->stream) {
            diagnostics.report(DiagCode::FileNotOpen, descriptor.location, "write", file);
            return;
        }
        // Whole bytes of the value's bits, the lowest first: Enzan's own format for a file of a
        // type, which a run reads back as it wrote it. An unconstrained array's count of elements
        // comes first.
        if (arguments.size() == 3) {
            const std::uint64_t count = arguments[2].value.toUInt64();
            std::array<char, 4> header{};
            for (std::size_t index = 0; index < header.size(); ++index)
                header.at(index) = static_cast<char>(count >> (8 * index) & 0xFFU);
            open->stream->write(header.data(), header.size());
        }
        const eir::ConstBitRef value = arguments[1].value;
        std::string bytes((value.width() + 7) / 8, '\0');
        for (std::uint32_t bit = 0; bit < value.width(); ++bit) {
            if (value.get(bit) == eir::Logic::One)
                bytes[bit / 8] = static_cast<char>(bytes[bit / 8] | (1 << (bit % 8)));
        }
        open->stream->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return;
    }

    case eir::SysTaskKind::Random:
    case eir::SysTaskKind::URandom:
    case eir::SysTaskKind::URandomRange:
    case eir::SysTaskKind::FileOpen:
    case eir::SysTaskKind::FileGetCharacter:
    case eir::SysTaskKind::LineWrite:
    case eir::SysTaskKind::LineWriteLine:
    case eir::SysTaskKind::LineReadLine:
    case eir::SysTaskKind::LineReadCharacter:
    case eir::SysTaskKind::LineReadString:
    case eir::SysTaskKind::LineLength:
    case eir::SysTaskKind::LineElement:
    case eir::SysTaskKind::LineSlice:
    case eir::SysTaskKind::FileEnd:
    case eir::SysTaskKind::LineAppend:
    case eir::SysTaskKind::FileReadValue:
    case eir::SysTaskKind::StringNew:
    case eir::SysTaskKind::StringBounds:
    case eir::SysTaskKind::StringSlice:
    case eir::SysTaskKind::StringFromArray:
    case eir::SysTaskKind::StringIntoArray:
    case eir::SysTaskKind::StringCompare:
    case eir::SysTaskKind::StringRead:
    case eir::SysTaskKind::StringMark:
        ENZAN_ASSERT(false, "a system function run as a system task; run the EIR verifier");
        return;
    }
}

void Kernel::runSystemFunction(std::uint32_t processIndex, const Activation& activation,
                               std::uint32_t sysCallIndex, eir::BitRef result) {
    ProcessState& state = processes[processIndex];
    const codegen::CompiledBody& body = *activation.body;
    const codegen::SysTaskDescriptor& descriptor = body.sysCalls[sysCallIndex];

    // An argument as a number; an unknown bit reads as 0, as other simulators read it.
    const auto argument = [&](std::size_t index) -> std::uint64_t {
        const codegen::SysTaskDescriptor::Argument& compiled = descriptor.arguments[index];
        const eir::ConstBitRef value =
            compiled.isConstant
                ? body.constant(compiled.location)
                : frameAt(activation.frameOffset + compiled.location, compiled.bitWidth);
        std::uint64_t number = value.aval()[0] & ~value.bval()[0];
        if (value.width() < 64) number &= (std::uint64_t{1} << value.width()) - 1;
        return number;
    };

    if (descriptor.kind == eir::SysTaskKind::FileOpen) {
        const auto text = [&](std::size_t index) {
            const codegen::SysTaskDescriptor::Argument& compiled = descriptor.arguments[index];
            return toAsciiString(
                compiled.isConstant
                    ? body.constant(compiled.location)
                    : frameAt(activation.frameOffset + compiled.location, compiled.bitWidth));
        };
        const std::uint32_t opened =
            openFile(text(0), descriptor.arguments.size() > 1 ? text(1) : std::string(),
                     descriptor.location);
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        result.aval()[0] = opened;
        result.canonicalize();
        return;
    }

    std::uint64_t value = 0;
    switch (descriptor.kind) {
    // std.textio over the heap: the line `write` appends to, justified in its field.
    case eir::SysTaskKind::LineWrite:
    case eir::SysTaskKind::LineAppend: {
        std::vector<FormatArgument> rest;
        for (std::size_t index = 3; index < descriptor.arguments.size(); ++index) {
            const codegen::SysTaskDescriptor::Argument& compiled = descriptor.arguments[index];
            const eir::ConstBitRef shown =
                compiled.isConstant
                    ? body.constant(compiled.location)
                    : frameAt(activation.frameOffset + compiled.location, compiled.bitWidth);
            rest.push_back(FormatArgument{
                .value = shown,
                .isSigned = compiled.isSigned,
                .isReal = compiled.isReal,
                .text = compiled.isString ? &scratchAt(shown.aval()[0]).text : nullptr});
        }
        const elab::Instance& instance =
            designImage->elaboratedDesign().instances()[state.instanceIndex];
        const std::string scope = elab::dottedPath(instance.path);
        const FormatContext context{.time = currentTime,
                                    .timeScale = options.timeScale,
                                    .argumentUnit = argumentUnit(descriptor),
                                    .scopeName = scope};
        std::string text;
        if (descriptor.kind == eir::SysTaskKind::LineAppend) {
            if (const std::string* source = lineAt(argument(3))) text = *source;
            freeLine(argument(3));
        } else {
            text = descriptor.hasFormat()
                       ? formatSystemTask(body.strings[descriptor.formatIndex], rest, context)
                       : formatDefault(rest);
        }
        const std::size_t field = argument(2);
        if (text.size() < field) {
            const std::string padding(field - text.size(), ' ');
            text = argument(1) != 0 ? text + padding : padding + text;
        }
        value = argument(0);
        if (std::string* line = lineAt(value)) {
            *line += text;
        } else {
            value = allocateLine(std::move(text));
        }
        break;
    }

    case eir::SysTaskKind::LineWriteLine: {
        const std::string* line = lineAt(argument(1));
        writeToDescriptor(static_cast<std::uint32_t>(argument(0)),
                          (line != nullptr ? *line : std::string()) + '\n', "writeline",
                          descriptor.location);
        freeLine(argument(1));
        value = allocateLine({});
        break;
    }

    case eir::SysTaskKind::LineReadLine: {
        freeLine(argument(1));
        std::string text;
        OpenFile* file = fileFor(static_cast<std::uint32_t>(argument(0)));
        if (file != nullptr && file->input) {
            std::getline(*file->input, text);
            if (text.ends_with('\r')) text.pop_back();
        } else {
            diagnostics.report(DiagCode::FileNotOpen, descriptor.location, "readline",
                               static_cast<std::uint32_t>(argument(0)));
        }
        value = allocateLine(std::move(text));
        break;
    }

    case eir::SysTaskKind::LineReadCharacter:
        if (std::string* line = lineAt(argument(0)); line != nullptr && !line->empty()) {
            value = 0x100U | static_cast<unsigned char>(line->front());
            line->erase(0, 1);
        } else if (descriptor.arguments.size() > 1 && argument(1) != 0) {
            report(eir::SeverityFailure, descriptor.location,
                   "a read of a character from an empty line");
        }
        break;

    case eir::SysTaskKind::LineReadString: {
        const codegen::SysTaskDescriptor::Argument& into = descriptor.arguments[1];
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        bitops::assignPartAt(result,
                             into.isConstant
                                 ? body.constant(into.location)
                                 : frameAt(activation.frameOffset + into.location, into.bitWidth),
                             0);
        const std::uint32_t count = (result.width() - 1) / 8;
        std::string* line = lineAt(argument(0));
        const std::size_t held = line != nullptr ? line->size() : 0;
        if (held < count) {
            if (descriptor.arguments.size() > 2 && argument(2) != 0) {
                report(eir::SeverityFailure, descriptor.location,
                       std::format("a read of {} characters from a line holding {}", count, held));
            }
            return;
        }
        if (line != nullptr) {
            for (std::uint32_t at = 0; at < count; ++at) {
                const auto character = static_cast<unsigned char>((*line)[at]);
                bitops::assignPartAt(result, eir::BitVector::fromUInt64(character, 8).ref(),
                                     std::uint64_t{count - 1 - at} * 8);
            }
            line->erase(0, count);
        }
        result.set(result.width() - 1, eir::Logic::One);
        return;
    }

    case eir::SysTaskKind::LineLength:
        if (const std::string* line = lineAt(argument(0))) {
            value = line->size();
        } else {
            report(eir::SeverityFailure, descriptor.location, "'length of a null line");
        }
        break;

    case eir::SysTaskKind::LineElement: {
        const auto index = static_cast<std::int64_t>(argument(1));
        if (const std::string* line = lineOrFailure(argument(0), index, 1, descriptor.location))
            value = static_cast<unsigned char>((*line)[static_cast<std::size_t>(index - 1)]);
        break;
    }

    case eir::SysTaskKind::LineSlice: {
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        const auto left = static_cast<std::int64_t>(argument(1));
        const std::uint32_t count = result.width() / 8;
        const std::string* line = lineOrFailure(argument(0), left, count, descriptor.location);
        if (line == nullptr) return;
        for (std::uint32_t at = 0; at < count; ++at) {
            const auto character =
                static_cast<unsigned char>((*line)[static_cast<std::size_t>(left - 1) + at]);
            bitops::assignPartAt(result, eir::BitVector::fromUInt64(character, 8).ref(),
                                 std::uint64_t{count - 1 - at} * 8);
        }
        return;
    }

    // A byte, or EOF, -1 in the 32 bits of an `int`.
    case eir::SysTaskKind::FileGetCharacter: {
        OpenFile* file = fileFor(static_cast<std::uint32_t>(argument(0)));
        const std::char_traits<char>::int_type next =
            file != nullptr && file->input ? file->input->get() : std::char_traits<char>::eof();
        value = next == std::char_traits<char>::eof()
                    ? 0xFFFF'FFFFU
                    : static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        break;
    }

    case eir::SysTaskKind::FileEnd: {
        OpenFile* file = fileFor(static_cast<std::uint32_t>(argument(0)));
        value =
            file == nullptr || !file->input || file->input->peek() == std::char_traits<char>::eof()
                ? 1
                : 0;
        break;
    }

    // The next value of a file of a type, as FileWriteValue wrote it; past the end, zero.
    case eir::SysTaskKind::FileReadValue: {
        OpenFile* file = fileFor(static_cast<std::uint32_t>(argument(0)));
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        if (file == nullptr || !file->input) {
            diagnostics.report(DiagCode::FileNotOpen, descriptor.location, "read",
                               static_cast<std::uint32_t>(argument(0)));
            return;
        }
        // How many bits the value has, and from which bit of the result its lowest goes: all of
        // the result, or for an unconstrained array the elements the header counts, the leftmost
        // as many as fit, placed from the left of the array read into, below how many they are.
        std::uint64_t valueBits = result.width();
        std::int64_t shift = 0;
        std::uint32_t valueTop = result.width();
        if (descriptor.arguments.size() == 3) {
            std::array<char, 4> header{};
            file->input->read(header.data(), header.size());
            std::uint64_t count = 0;
            for (std::size_t index = 0; index < header.size(); ++index)
                count |= std::uint64_t{static_cast<unsigned char>(header.at(index))} << (8 * index);
            const std::uint64_t elementBits = std::max<std::uint64_t>(argument(1), 1);
            valueTop = result.width() - 32;
            const codegen::SysTaskDescriptor::Argument& into = descriptor.arguments[2];
            bitops::assignPartAt(
                result,
                into.isConstant ? body.constant(into.location)
                                : frameAt(activation.frameOffset + into.location, into.bitWidth),
                0);
            valueBits = count * elementBits;
            shift = static_cast<std::int64_t>(valueTop) - static_cast<std::int64_t>(valueBits);
            const std::uint64_t read = std::min<std::uint64_t>(count, valueTop / elementBits);
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                result.set(valueTop + bit,
                           (read >> bit & 1U) != 0 ? eir::Logic::One : eir::Logic::Zero);
            }
        }
        std::string bytes((valueBits + 7) / 8, '\0');
        file->input->read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        for (std::uint64_t bit = 0; bit < valueBits; ++bit) {
            const std::int64_t to = static_cast<std::int64_t>(bit) + shift;
            if (to < 0 || std::cmp_greater_equal(to, valueTop)) continue;
            const bool one = (static_cast<unsigned char>(bytes[bit / 8]) >> (bit % 8) & 1U) != 0;
            result.set(static_cast<std::uint32_t>(to), one ? eir::Logic::One : eir::Logic::Zero);
        }
        return;
    }

    // Strings whose lengths only the run knows, in the scratch the running process makes them in.
    case eir::SysTaskKind::StringNew: {
        const auto left = static_cast<std::int64_t>(argument(0));
        const auto right = static_cast<std::int64_t>(argument(1));
        const bool ascending = argument(2) != 0;
        const std::int64_t span = ascending ? right - left : left - right;
        std::uint64_t length = span < 0 ? 0 : static_cast<std::uint64_t>(span) + 1;
        if (length > MaxStringLength) {
            report(eir::SeverityFailure, descriptor.location,
                   std::format("a string of {} characters, more than {}", length, MaxStringLength));
            length = 0;
        }
        scratch.push_back(
            ScratchString{.left = left,
                          .right = right,
                          .ascending = ascending,
                          .text = std::string(length, static_cast<char>(argument(3) & 0xFFU))});
        value = scratch.size();
        break;
    }

    case eir::SysTaskKind::StringBounds: {
        const ScratchString& string = scratchAt(argument(0));
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        const auto put = [&](std::uint64_t number, std::uint64_t lowBit) {
            bitops::assignPartAt(result, eir::BitVector::fromUInt64(number, 64).ref(), lowBit);
        };
        put(static_cast<std::uint64_t>(string.left), 128);
        put(static_cast<std::uint64_t>(string.right), 64);
        put(string.text.size(), 0);
        return;
    }

    case eir::SysTaskKind::StringSlice: {
        std::ranges::fill(result.avalWords(), 0);
        std::ranges::fill(result.bvalWords(), 0);
        const ScratchString& string = scratchAt(argument(0));
        const auto index = static_cast<std::int64_t>(argument(1));
        const std::uint32_t count = result.width() / 8;
        if (!withinOrFailure(string, index, count, descriptor.location)) return;
        const auto from = static_cast<std::size_t>(string.position(index));
        for (std::uint32_t at = 0; at < count; ++at)
            putCharacter(result, std::uint64_t{count - 1 - at} * 8, string.text[from + at]);
        return;
    }

    // A slice of an array, whose bounds the run gives, as a string of its own; its elements are
    // where they are in the array, the right bound's lowest.
    case eir::SysTaskKind::StringFromArray:
    case eir::SysTaskKind::StringIntoArray: {
        const codegen::SysTaskDescriptor::Argument& compiled = descriptor.arguments[0];
        const eir::ConstBitRef array =
            compiled.isConstant
                ? body.constant(compiled.location)
                : frameAt(activation.frameOffset + compiled.location, compiled.bitWidth);
        const auto arrayLeft = static_cast<std::int64_t>(argument(1));
        const auto arrayRight = static_cast<std::int64_t>(argument(2));
        const auto offsetOf = [&](std::int64_t index, bool ascending) {
            return static_cast<std::uint64_t>(ascending ? arrayRight - index : index - arrayRight)
                   * 8;
        };
        if (descriptor.kind == eir::SysTaskKind::StringIntoArray) {
            const ScratchString& string = scratchAt(argument(3));
            bitops::assignPartAt(result, array, 0);
            for (std::size_t at = 0; at < string.text.size(); ++at) {
                const auto step = static_cast<std::int64_t>(at);
                const std::int64_t index =
                    string.ascending ? string.left + step : string.left - step;
                putCharacter(result, offsetOf(index, string.ascending), string.text[at]);
            }
            return;
        }
        const auto left = static_cast<std::int64_t>(argument(3));
        const auto right = static_cast<std::int64_t>(argument(4));
        const bool ascending = argument(5) != 0;
        const std::int64_t span = ascending ? right - left : left - right;
        const std::uint64_t length = span < 0 ? 0 : static_cast<std::uint64_t>(span) + 1;
        ScratchString string{.left = left,
                             .right = right,
                             .ascending = ascending,
                             .text = std::string(length, '\0')};
        const std::int64_t arrayLow = std::min(arrayLeft, arrayRight);
        const std::int64_t arrayHigh = std::max(arrayLeft, arrayRight);
        if (length > 0 && (std::min(left, right) < arrayLow || std::max(left, right) > arrayHigh)) {
            const std::string_view direction = ascending ? "to" : "downto";
            report(eir::SeverityFailure, descriptor.location,
                   std::format("{} {} {} is outside the bounds {} {} {}", left, direction, right,
                               arrayLeft, direction, arrayRight));
        } else {
            for (std::uint64_t at = 0; at < length; ++at) {
                const auto step = static_cast<std::int64_t>(at);
                string.text[at] =
                    characterAt(array, offsetOf(ascending ? left + step : left - step, ascending));
            }
        }
        scratch.push_back(std::move(string));
        value = scratch.size();
        break;
    }

    // Character by character, as a VHDL array of characters orders by their positions, and a
    // prefix first (IEEE 1076 section 9.2.3); std::string compares its characters unsigned.
    case eir::SysTaskKind::StringCompare: {
        const int order = scratchAt(argument(0)).text.compare(scratchAt(argument(1)).text);
        if (order < 0) value = 0xFFU;
        if (order > 0) value = 1U;
        break;
    }

    case eir::SysTaskKind::StringRead: {
        ScratchString& string = scratchAt(argument(0));
        std::string* line = lineAt(argument(1));
        const std::size_t count = string.text.size();
        const std::size_t held = line != nullptr ? line->size() : 0;
        if (held < count) {
            if (descriptor.arguments.size() > 2 && argument(2) != 0) {
                report(eir::SeverityFailure, descriptor.location,
                       std::format("a read of {} characters from a line holding {}", count, held));
            }
            break;
        }
        if (line != nullptr) {
            string.text.assign(*line, 0, count);
            line->erase(0, count);
        }
        value = 1;
        break;
    }

    case eir::SysTaskKind::StringMark: value = scratch.size(); break;

    case eir::SysTaskKind::Random:
        if (descriptor.arguments.empty()) {
            value = static_cast<std::uint32_t>(ieeeRandom(randomSeed));
        } else {
            auto seed = static_cast<std::int32_t>(static_cast<std::uint32_t>(argument(0)));
            const std::int32_t draw = ieeeRandom(seed);
            value = (std::uint64_t{static_cast<std::uint32_t>(seed)} << 32U)
                    | static_cast<std::uint32_t>(draw);
        }
        break;

    case eir::SysTaskKind::URandom:
        if (!descriptor.arguments.empty())
            state.randomState = processRandomState(static_cast<std::uint32_t>(argument(0)), {});
        value = splitMix(state.randomState) & 0xFFFF'FFFFU;
        break;

    case eir::SysTaskKind::URandomRange: {
        std::uint64_t high = argument(0) & 0xFFFF'FFFFU;
        std::uint64_t low = descriptor.arguments.size() > 1 ? argument(1) & 0xFFFF'FFFFU : 0;
        if (low > high) std::swap(low, high);
        value = low + (splitMix(state.randomState) % (high - low + 1));
        break;
    }

    default: ENZAN_ASSERT(false, "a system task run as a system function"); break;
    }

    std::ranges::fill(result.avalWords(), 0);
    std::ranges::fill(result.bvalWords(), 0);
    result.aval()[0] = value;
    result.canonicalize();
}

void Kernel::readMemory(std::span<const std::uint32_t> objectSignals,
                        const codegen::SysTaskDescriptor& descriptor,
                        std::span<const FormatArgument> arguments) {
    const std::string task(eir::toString(descriptor.kind));
    const std::string name = toAsciiString(arguments[0].value);
    const auto address = [&](std::size_t index) -> std::optional<std::int64_t> {
        if (arguments.size() <= index || arguments[index].value.hasUnknown()) return std::nullopt;
        return arguments[index].isSigned
                   ? arguments[index].value.toInt64()
                   : static_cast<std::int64_t>(arguments[index].value.toUInt64());
    };

    const std::ifstream file(name);
    if (!file) {
        diagnostics.report(DiagCode::ReadMemProblem, descriptor.location, task, name,
                           "the file could not be opened");
        return;
    }
    std::ostringstream contents;
    contents << file.rdbuf();

    // The memory as it is, so that what the file does not reach keeps its value, written back
    // whole: one change, which wakes what reads the memory once.
    const std::uint32_t signalIndex = objectSignals[descriptor.objectIndex];
    const elab::Slot& slot = designImage->elaboratedDesign().slots()[signalIndex];
    const Signal& signal = designImage->signals()[signalIndex];
    eir::BitVector memory = eir::BitVector::from(stateArena.at(signal.wordOffset, signal.bitWidth));
    const MemoryShape shape{.left = slot.unpackedDimensions.front().left,
                            .right = slot.unpackedDimensions.front().right,
                            .elementWidth = slot.elementWidth};

    for (const std::string& problem :
         loadMemoryImage(contents.str(), descriptor.kind == eir::SysTaskKind::ReadMemHex, shape,
                         address(1), address(2), memory.mutableRef()))
        diagnostics.report(DiagCode::ReadMemProblem, descriptor.location, task, name, problem);
    writeSignal(signalIndex, memory.ref());
}

std::uint32_t Kernel::openFile(const std::string& name, const std::string& mode,
                               SourceLocation location) {
    // Writing, or reading, for `$fgetc` and for VHDL's textio.
    const bool channel = mode.empty();
    if (mode.starts_with('r')) {
        auto input = std::make_unique<std::ifstream>(name);
        if (!input->is_open()) {
            diagnostics.report(DiagCode::FileNotOpened, location, name, mode,
                               "the file could not be opened");
            return 0;
        }
        const std::uint32_t descriptor = 0x8000'0000U | nextFileNumber++;
        openFiles.push_back(
            OpenFile{.descriptor = descriptor, .stream = {}, .input = std::move(input)});
        return descriptor;
    }
    if (!channel && !mode.starts_with('w') && !mode.starts_with('a')) {
        diagnostics.report(DiagCode::FileNotOpened, location, name, mode,
                           "Enzan opens files for writing, or for textio to read");
        return 0;
    }
    if (channel && nextChannel > 30) {
        diagnostics.report(DiagCode::FileNotOpened, location, name, mode,
                           "all 30 channels of a multichannel descriptor are in use");
        return 0;
    }

    auto stream = std::make_unique<std::ofstream>(
        name, mode.starts_with('a') ? std::ios::out | std::ios::app : std::ios::out);
    if (!stream->is_open()) {
        diagnostics.report(DiagCode::FileNotOpened, location, name, mode,
                           "the file could not be created");
        return 0;
    }

    const std::uint32_t descriptor =
        channel ? std::uint32_t{1} << nextChannel++ : 0x8000'0000U | nextFileNumber++;
    openFiles.push_back(
        OpenFile{.descriptor = descriptor, .stream = std::move(stream), .input = {}});
    return descriptor;
}

void Kernel::report(std::uint64_t severity, const SourceLocation& location,
                    const std::string& text) {
    switch (severity) {
    case eir::SeverityNote: diagnostics.report(DiagCode::UserInfo, location, text); break;
    case eir::SeverityWarning: diagnostics.report(DiagCode::UserWarning, location, text); break;
    case eir::SeverityError: diagnostics.report(DiagCode::UserError, location, text); break;
    default: diagnostics.report(DiagCode::AssertionFailure, location, text); break;
    }
    if (severity >= options.stopSeverity) {
        finishRequested = true;
        errorRequested = true;
        finishStatus = 1;
    }
}

const std::string* Kernel::lineOrFailure(std::uint64_t handle, std::int64_t index,
                                         std::uint32_t count, const SourceLocation& location) {
    const std::string* line = lineAt(handle);
    if (line == nullptr) {
        report(eir::SeverityFailure, location, "a null line is indexed");
        return nullptr;
    }
    const auto length = static_cast<std::int64_t>(line->size());
    if (index < 1 || index + count - 1 > length) {
        const std::string reached = count == 1 ? std::format("index {}", index)
                                               : std::format("{} to {}", index, index + count - 1);
        report(eir::SeverityFailure, location,
               std::format("{} is outside the bounds 1 to {} of the line", reached, length));
        return nullptr;
    }
    return line;
}

Kernel::ScratchString& Kernel::scratchAt(std::uint64_t handle) {
    ENZAN_ALWAYS_ASSERT(handle != 0 && handle <= scratch.size(),
                        "a string of run-time length used once released");
    return scratch[handle - 1];
}

void Kernel::releaseScratch(std::uint64_t mark) {
    if (mark < scratch.size()) scratch.resize(mark);
}

bool Kernel::withinOrFailure(const ScratchString& string, std::int64_t index, std::uint32_t count,
                             const SourceLocation& location) {
    const std::int64_t from = string.position(index);
    if (count == 0
        || (from >= 0 && std::cmp_less_equal(from + std::int64_t{count}, string.text.size())))
        return true;
    const std::string_view direction = string.ascending ? "to" : "downto";
    const std::int64_t last = string.ascending ? index + count - 1 : index - count + 1;
    const std::string reached = count == 1 ? std::format("index {}", index)
                                           : std::format("{} {} {}", index, direction, last);
    report(eir::SeverityFailure, location,
           std::format("{} is outside the bounds {} {} {}", reached, string.left, direction,
                       string.right));
    return false;
}

std::uint64_t Kernel::allocateLine(std::string text) {
    if (!freedHandles.empty()) {
        const std::uint64_t handle = freedHandles.back();
        freedHandles.pop_back();
        heap[handle - 1] = std::move(text);
        return handle;
    }
    heap.emplace_back(std::move(text));
    return heap.size();
}

std::string* Kernel::lineAt(std::uint64_t handle) {
    if (handle == 0 || handle > heap.size()) return nullptr;
    std::optional<std::string>& entry = heap[handle - 1];
    return entry.has_value() ? &entry.value() : nullptr;
}

void Kernel::freeLine(std::uint64_t handle) {
    if (lineAt(handle) == nullptr) return;
    heap[handle - 1].reset();
    freedHandles.push_back(handle);
}

Kernel::OpenFile* Kernel::fileFor(std::uint32_t descriptor) {
    const auto found = std::ranges::find(openFiles, descriptor, &OpenFile::descriptor);
    return found != openFiles.end() ? &*found : nullptr;
}

void Kernel::writeToDescriptor(std::uint32_t descriptor, std::string_view text,
                               std::string_view task, SourceLocation location) {
    if ((descriptor & 0x8000'0000U) != 0) {
        // A file descriptor: the standard streams, or one file.
        if (descriptor == 0x8000'0001U) {
            transcriptStream() << text;
        } else if (descriptor == 0x8000'0002U) {
            std::cerr << text;
        } else if (OpenFile* file = fileFor(descriptor); file != nullptr && file->stream) {
            *file->stream << text;
        } else {
            diagnostics.report(DiagCode::FileNotOpen, location, task, descriptor);
        }
        return;
    }

    // A multichannel descriptor: bit 0 is the standard output, each other bit one file.
    if ((descriptor & 1U) != 0) transcriptStream() << text;
    for (OpenFile& file : openFiles) {
        if ((file.descriptor & 0x8000'0000U) == 0 && (descriptor & file.descriptor) != 0
            && file.stream)
            *file.stream << text;
    }
}

void Kernel::reportDeltaLimit() {
    // The diagnostic names the processes and signals still active: a delta loop with no
    // diagnostic is one of the most frustrating failures a user can hit, and the names
    // are the only way to find the loop.
    std::string active;
    unsigned listed = 0;
    for (const ProcessState& state : processes) {
        if (!state.queued) continue;
        if (listed == MaxProcessesInDeltaReport) {
            active += ", ...";
            break;
        }
        if (!active.empty()) active += ", ";
        active += state.name;
        ++listed;
    }

    if (active.empty())
        active = "(no process is queued; the loop is in the non-blocking update queue)";

    diagnostics.report(DiagCode::DeltaLimitExceeded, SourceLocation(), options.deltaLimit,
                       options.timeScale.format(currentTime), active);
}

} // namespace enzan::runtime
