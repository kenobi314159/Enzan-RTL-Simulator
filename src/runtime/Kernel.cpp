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

#include <algorithm>
#include <format>
#include <iostream>
#include <string>
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

} // namespace

Kernel::Kernel(const Image& designImageToRun, DiagnosticEngine& engine, KernelOptions kernelOptions)
    : designImage(&designImageToRun),
      diagnostics(engine),
      options(kernelOptions),
      stateArena(designImageToRun.arenaWords()),
      frameStorage(designImageToRun.frameWords(), 0),
      processes(designImageToRun.processes().begin(), designImageToRun.processes().end()),
      edgeWaiters(designImageToRun.signals().size()) {}

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
    processes.assign(designImage->processes().begin(), designImage->processes().end());
    for (std::vector<EdgeWaiter>& waiters : edgeWaiters) waiters.clear();
    std::ranges::fill(frameStorage, eir::Word{});

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
        const codegen::CompiledProcess& body =
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

        if (!scheduler.allRegionsEmpty() || scheduler.hasNonBlockingUpdates() || pliWorkDue()) {
            if (!runTimeSlot()) {
                lastStatus = errorRequested ? RunStatus::Error : RunStatus::Finished;
                runFinalProcesses();
                return lastStatus;
            }
        }

        if (!advanceTime(until)) return lastStatus;
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

        if (scheduler.hasNonBlockingUpdates()) {
            applyNonBlockingUpdates();
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

        const ExecutionResult result = interpreter.run(processIndex);

        if (result == ExecutionResult::Stopped || stopping()) return;

        if (result != ExecutionResult::Completed) continue;

        const codegen::CompiledProcess& body =
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
    const SimTime next = std::min(scheduler.nextEventTime(),
                                  pliHooks != nullptr ? pliHooks->nextTimedEvent() : NoTime);

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
        const codegen::CompiledProcess& body =
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

    const eir::Logic before = target.get(0);
    bitops::extend(target, value, false);
    const eir::Logic after = target.get(0);

    for (ValueChangeListener* listener : listeners) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after);

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

    const eir::Logic before = target.get(0);
    bitops::assignPartAt(target, value, lowBit);
    const eir::Logic after = target.get(0);

    for (ValueChangeListener* listener : listeners) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after);

    if (pliHooks != nullptr) pliHooks->valueChanged(signalIndex);
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

void Kernel::notifyChange(std::uint32_t signalIndex, eir::Logic before, eir::Logic after) {
    for (const Signal::Subscriber& subscriber : designImage->signals()[signalIndex].subscribers) {
        if (edgeMatches(subscriber.edge, before, after)) wake(subscriber.processIndex);
    }

    // Dynamic subscriptions are consumed when they fire: a process that waited for one
    // edge is not waiting for the next until it asks again. Rewritten in place rather
    // than erased one by one, so that the common case of nothing matching costs one pass.
    std::vector<EdgeWaiter>& waiters = edgeWaiters[signalIndex];
    std::size_t kept = 0;
    for (std::size_t index = 0; index < waiters.size(); ++index) {
        const EdgeWaiter waiter = waiters[index];
        if (edgeMatches(waiter.edge, before, after)) {
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

void Kernel::suspendForEdge(std::uint32_t processIndex, std::uint32_t signalIndex,
                            eir::EdgeKind edge, std::uint32_t resumePc) {
    processes[processIndex].resumePc = resumePc;
    edgeWaiters[signalIndex].push_back(
        EdgeWaiter{.processIndex = processIndex, .edge = edge, .resumePc = resumePc});
}

void Kernel::runSystemTask(std::uint32_t processIndex, std::uint32_t sysCallIndex) {
    const ProcessState& state = processes[processIndex];
    const codegen::CompiledProcess& body = designImage->compiledDesign().body(state.bodyIndex);
    const codegen::SysTaskDescriptor& descriptor = body.sysCalls[sysCallIndex];

    std::vector<FormatArgument> arguments;
    arguments.reserve(descriptor.arguments.size());
    for (const codegen::SysTaskDescriptor::Argument& argument : descriptor.arguments) {
        const eir::ConstBitRef value =
            argument.isConstant ? body.constant(argument.location)
                                : frameAt(state.frameOffset + argument.location, argument.bitWidth);
        arguments.push_back(FormatArgument{.value = value, .isSigned = argument.isSigned});
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

    case eir::SysTaskKind::DumpFile:
        for (ValueChangeListener* listener : listeners) listener->onDumpFile(text);
        return;

    case eir::SysTaskKind::DumpVars:
        for (ValueChangeListener* listener : listeners) listener->onDumpVars();
        return;
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
