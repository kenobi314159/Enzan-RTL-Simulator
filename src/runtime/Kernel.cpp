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

/// The words a windowed signal's old value needs, for the widest such signal.
std::size_t windowScratchWords(const Image& image) {
    std::uint32_t widest = 0;
    for (const Signal& signal : image.signals())
        if (signal.windowed) widest = std::max(widest, signal.bitWidth);
    return eir::storageWordsFor(widest);
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
}

Kernel::~Kernel() = default;

std::ostream& Kernel::transcriptStream() const {
    return options.transcript != nullptr ? *options.transcript : std::cout;
}

void Kernel::initialize() {
    designImage->initializeState(stateArena);

    // Everything a previous run left behind goes with it, because a restart is this method
    // called a second time and a design put back to time zero must behave exactly
    // as it did the first time. A clock event still in the time wheel would resume at a
    // timestamp the design has not reached; a process still holding a resume point would
    // continue a simulation that no longer exists; an edge subscription would fire for an
    // edge nothing is waiting for any more. Each of those produces a design that runs and
    // gives a different answer, which section 1.2 ranks as the worst possible outcome.
    scheduler.clear();
    delayedWrites.clear();
    pendingInertial.clear();
    deltaTransactions.clear();
    for (std::vector<Transaction>& waveform : projectedWaveforms) waveform.clear();
    processes.assign(designImage->processes().begin(), designImage->processes().end());
    for (ProcessState& state : processes)
        state.randomState = processRandomState(options.seed, state.name);
    randomSeed = static_cast<std::int32_t>(options.seed);
    // Files a previous run opened are closed, which flushes what it wrote, and are opened
    // again, from the start, by the run that follows.
    openFiles.clear();
    nextFileNumber = 3;
    nextChannel = 1;
    for (EdgeWaiterList& list : edgeWaiters) {
        list.waiters.clear();
        list.sweepAt = MinimumWaiterSweep;
    }
    std::ranges::fill(frameStorage, eir::Word{});
    unwindCalls();

    // A force does not survive a restart either: a signal still pinned from the last run would
    // be state the user did not ask to keep.
    forcedSignals.clear();

    finishRequested = false;
    errorRequested = false;
    stopRequested = false;
    finishStatus = 0;
    finalProcessesRun = false;
    lastStatus = RunStatus::Quiescent;

    currentTime = 0;

    for (std::uint32_t processIndex = 0; processIndex < processes.size(); ++processIndex) {
        const codegen::CompiledBody& body =
            designImage->compiledDesign().body(processes[processIndex].bodyIndex);
        if (body.startsAtTimeZero) wake(processIndex);
    }
}

RunStatus Kernel::run(SimTime until) {
    while (true) {
        // Asked here, between time slots, because that is the only point at which the whole of
        // the run's state is in the scheduler and the arena and nothing is half-done. Stopping
        // anywhere else would make resuming produce a different answer from running straight
        // through, which section 3.8 forbids.
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
            || !deltaTransactions.empty() || pliWorkDue()) {
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
    // The region iteration rules of DESIGN_GUIDE.md section 3.3. Active is drained
    // fully; only when Active, Inactive and NBA are all empty does the slot move on to
    // Observed, and the Reactive family iterates by the same rule. Anything that
    // schedules Active work sends control back to the top.
    //
    // An application's timed events run first, before the Active region is drained, and its
    // read-write callbacks once everything else has settled (section 9.3).
    bool firstPass = true;
    while (true) {
        if (pliHooks != nullptr && pliHooks->nextTimedEvent() <= currentTime) {
            // On any pass but the first, what is due was scheduled during this slot with no
            // delay, and running it is a further iteration of the slot. Counting it keeps an
            // application that does that forever inside the delta limit of section 3.2.
            if (!firstPass && !countDelta()) return false;
            pliHooks->runTimedEvents(currentTime);
            if (halted()) return false;
        }
        firstPass = false;

        drain(Region::Active);
        if (halted()) return false;

        if (!scheduler.empty(Region::Inactive)) {
            scheduler.promote(Region::Inactive, Region::Active);
            if (!countDelta()) return false;
            continue;
        }

        // VHDL's transactions for the next delta cycle are applied with SystemVerilog's NBA
        // updates: either way, after every process of this iteration has run (section 3.5).
        if (scheduler.hasNonBlockingUpdates() || !deltaTransactions.empty()) {
            applyNonBlockingUpdates();
            if (halted()) return false;
            applyDeltaTransactions();
            if (halted()) return false;
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
    // refused while it runs (section 9.4), so it cannot send control back.
    if (pliHooks != nullptr && pliHooks->hasReadOnly()) {
        pliHooks->runReadOnly();
        if (halted()) return false;
    }

    // Postponed is last and, in the M1 subset, cannot schedule anything: `$monitor` is
    // read-only by definition.
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
                                   delayedWrites.empty() ? NoTime : delayedWrites.begin()->first});

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
    scheduler.schedule(Region::Active, processIndex);
}

void Kernel::resume(std::uint32_t processIndex, std::uint32_t resumePc) {
    ProcessState& state = processes[processIndex];
    state.resumePc = resumePc;

    if (state.queued) return;

    state.queued = true;
    scheduler.schedule(Region::Active, processIndex);
}

void Kernel::writeSignal(std::uint32_t signalIndex, eir::ConstBitRef value) {
    if (isForced(signalIndex)) return;

    const Signal& signal = designImage->signals()[signalIndex];
    const eir::BitRef target = stateArena.at(signal.wordOffset, signal.bitWidth);

    // Value-change filtering before scheduling. Section 6.2 calls this one of the
    // largest single performance wins available and requires it from the start: a
    // process is woken when a signal it watches actually changes, not merely when it is
    // assigned.
    if (value.width() == target.width() && eir::ConstBitRef(target).identicalTo(value)) return;

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

void Kernel::writeSignalPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                             std::uint64_t lowBit) {
    if (isForced(signalIndex)) return;

    const Signal& signal = designImage->signals()[signalIndex];
    const eir::BitRef target = stateArena.at(signal.wordOffset, signal.bitWidth);

    // Change detection over the written window only, compared bit by bit against the
    // incoming value so that no temporary copy of the old value is needed on the hot
    // path (section 13.4).
    bool changed = false;
    for (std::uint32_t index = 0; index < value.width(); ++index) {
        const std::uint64_t position = lowBit + index;
        if (position >= target.width()) break;
        if (target.get(static_cast<std::uint32_t>(position)) != value.get(index)) {
            changed = true;
            break;
        }
    }
    if (!changed) return;

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
        bool changed = false;
        for (std::uint32_t bit = 0; bit < value.width() && offset + bit < driver.width; ++bit) {
            if (storage.get(static_cast<std::uint32_t>(offset + bit)) != value.get(bit)) {
                changed = true;
                break;
            }
        }
        if (!changed) return;

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
        const eir::BitRef next(driverScratch.data(), segment.width);

        const auto sliceOf = [&](std::uint32_t segmentDriver, eir::BitRef into) {
            const elab::Driver& driver =
                elaborated.drivers()[elaborated.segmentDrivers()[segmentDriver]];
            bitops::partSelectAt(into, stateArena.at(driver.wordOffset, driver.width),
                                 segment.lowBit - driver.lowBit);
        };

        sliceOf(segment.firstDriver, acc);
        for (std::uint32_t other = segment.firstDriver + 1;
             other < segment.firstDriver + segment.driverCount; ++other) {
            sliceOf(other, next);
            switch (segment.netKind) {
            case eir::NetKind::WiredAnd:
            case eir::NetKind::TriAnd: bitops::resolveWiredAnd(acc, next); break;
            case eir::NetKind::WiredOr:
            case eir::NetKind::TriOr: bitops::resolveWiredOr(acc, next); break;
            case eir::NetKind::Wire:
            case eir::NetKind::Tri:
            case eir::NetKind::Tri0:
            case eir::NetKind::Tri1:
            case eir::NetKind::Supply0:
            case eir::NetKind::Supply1:
            case eir::NetKind::UWire: bitops::resolveWire(acc, next); break;
            }
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

        bitops::assignPartAt(resolved, acc, segment.lowBit - written.lowBit);
    }

    writeSignalPart(written.slot, resolved, written.lowBit);
}

void Kernel::scheduleNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value) {
    scheduler.appendNonBlocking(signalIndex, value, 0, false);
}

void Kernel::scheduleNonBlockingPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                                     std::uint64_t lowBit) {
    scheduler.appendNonBlocking(signalIndex, value, static_cast<std::uint32_t>(lowBit), true);
}

bool Kernel::isForced(std::uint32_t signalIndex) const noexcept {
    return !forcedSignals.empty() && forcedSignals[signalIndex];
}

void Kernel::deposit(std::uint32_t signalIndex, eir::ConstBitRef value) {
    writeSignal(signalIndex, value);
}

void Kernel::force(std::uint32_t signalIndex, eir::ConstBitRef value) {
    // Cleared first so that re-forcing an already-forced signal to a different value writes
    // rather than being discarded by the force it is replacing.
    if (!forcedSignals.empty()) forcedSignals[signalIndex] = false;

    writeSignal(signalIndex, value);

    if (forcedSignals.empty()) forcedSignals.assign(designImage->signals().size(), false);
    forcedSignals[signalIndex] = true;
}

bool Kernel::release(std::uint32_t signalIndex) {
    if (!isForced(signalIndex)) return false;
    forcedSignals[signalIndex] = false;
    return true;
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
        for (std::uint32_t bit = lowBit; bit < lowBit + width; ++bit)
            if (previous.get(bit) != current.get(bit)) return true;
        return false;
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
        // `#0` resumes in this timestamp but after everything currently Active, which is
        // exactly what the Inactive region is for (section 3.3).
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
    scheduler.scheduleAt(currentTime + delay, processIndex);
}

void Kernel::scheduleDelayedWrite(std::uint32_t processIndex, std::uint32_t position,
                                  std::uint32_t signalIndex, eir::ConstBitRef value, SimTime delay,
                                  bool nonBlocking) {
    if (delay > NoTime - currentTime) {
        diagnostics.report(DiagCode::TimeOverflow, SourceLocation(), currentTime,
                           options.timeScale.unitName());
        errorRequested = true;
        return;
    }

    if (nonBlocking && delay == 0) {
        scheduler.appendNonBlocking(signalIndex, value, 0, false);
        return;
    }

    DelayedWrite write{.processIndex = processIndex,
                       .position = position,
                       .signalIndex = signalIndex,
                       .nonBlocking = nonBlocking,
                       .serial = ++nextWriteSerial,
                       .value = eir::BitVector::from(value)};

    if (!nonBlocking) {
        // The write this one replaces is taken out rather than skipped when it comes due, so
        // that it leaves no time step behind in which nothing happens.
        const auto key = std::pair{processIndex, position};
        if (const auto pending = pendingInertial.find(key); pending != pendingInertial.end()) {
            const auto slot = delayedWrites.find(pending->second.time);
            std::erase_if(slot->second, [&pending](const DelayedWrite& queued) {
                return queued.serial == pending->second.serial;
            });
            if (slot->second.empty()) delayedWrites.erase(slot);
            pendingInertial.erase(pending);
        }
        if (delay == 0) {
            writeRouted(processIndex, position, signalIndex, value);
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
            scheduler.appendNonBlocking(write.signalIndex, write.value.ref(), 0, false);
            continue;
        }
        pendingInertial.erase(std::pair{write.processIndex, write.position});
        writeRouted(write.processIndex, write.position, write.signalIndex, write.value.ref());
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

void Kernel::applyDeltaTransactions() {
    // Taken as a batch, as non-blocking updates are: a value-change callback may assign, and
    // what it assigns belongs to the delta cycle after this one.
    const std::vector<DeltaTransaction> due = std::exchange(deltaTransactions, {});
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
                         std::uint32_t signalIndex, eir::ConstBitRef value) {
    const WriteRoute& route = processes[processIndex].writeRoutes[position];
    if (route.direct()) {
        writeSignal(signalIndex, value);
    } else {
        writeThroughDrivers(route, value, 0);
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
    // can be read by this one (section 3.8).
    const std::uint32_t frame = callStackTop;
    callStackTop += body.frameWords;
    std::fill(frameStorage.begin() + frame, frameStorage.begin() + callStackTop, eir::Word{});

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
        arguments.push_back(FormatArgument{.value = value, .isSigned = argument.isSigned});
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
    const FormatContext context{
        .time = currentTime, .timeScale = options.timeScale, .scopeName = instance.path};

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

    // The positions of std.standard.severity_level: note, warning, error, failure.
    case eir::SysTaskKind::Report:
        switch (severity) {
        case 0: diagnostics.report(DiagCode::UserInfo, descriptor.location, text); return;
        case 1: diagnostics.report(DiagCode::UserWarning, descriptor.location, text); return;
        case 2: diagnostics.report(DiagCode::UserError, descriptor.location, text); return;
        default:
            diagnostics.report(DiagCode::AssertionFailure, descriptor.location, text);
            finishRequested = true;
            errorRequested = true;
            finishStatus = 1;
            return;
        }

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
            for (OpenFile& file : openFiles) file.stream->flush();
        } else if (OpenFile* file = fileFor(fileDescriptor)) {
            file->stream->flush();
        } else {
            transcriptStream().flush();
        }
        return;

    case eir::SysTaskKind::ReadMemHex:
    case eir::SysTaskKind::ReadMemBinary:
        readMemory(activation.objectSignals, descriptor, arguments);
        return;

    case eir::SysTaskKind::Random:
    case eir::SysTaskKind::URandom:
    case eir::SysTaskKind::URandomRange:
    case eir::SysTaskKind::FileOpen:
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
    // Writing only: a file opened for reading has nothing that reads it yet, and opening one
    // anyway would let a testbench read as if it had read nothing.
    const bool channel = mode.empty();
    if (!channel && !mode.starts_with('w') && !mode.starts_with('a')) {
        diagnostics.report(DiagCode::FileNotOpened, location, name, mode,
                           "Enzan opens files for writing only");
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
    openFiles.push_back(OpenFile{.descriptor = descriptor, .stream = std::move(stream)});
    return descriptor;
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
        } else if (OpenFile* file = fileFor(descriptor)) {
            *file->stream << text;
        } else {
            diagnostics.report(DiagCode::FileNotOpen, location, task, descriptor);
        }
        return;
    }

    // A multichannel descriptor: bit 0 is the standard output, each other bit one file.
    if ((descriptor & 1U) != 0) transcriptStream() << text;
    for (OpenFile& file : openFiles) {
        if ((file.descriptor & 0x8000'0000U) == 0 && (descriptor & file.descriptor) != 0)
            *file.stream << text;
    }
}

void Kernel::reportDeltaLimit() {
    // Section 3.2 requires the diagnostic to name the processes and signals still
    // active: a delta loop with no diagnostic is one of the most frustrating failures a
    // user can hit, and the names are the only way to find the loop.
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
