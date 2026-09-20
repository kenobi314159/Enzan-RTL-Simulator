// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Kernel.h"

#include "codegen/Bytecode.h"
#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "eir/Process.h"
#include "runtime/Format.h"
#include "runtime/Interpreter.h"

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

    currentTime = 0;
    scheduler.resetDeltaCount();

    for (std::uint32_t processIndex = 0; processIndex < processes.size(); ++processIndex) {
        const codegen::CompiledProcess& body =
            designImage->compiledDesign().body(processes[processIndex].bodyIndex);
        if (body.startsAtTimeZero) wake(processIndex);
    }
}

RunStatus Kernel::run(SimTime until) {
    while (true) {
        if (!scheduler.allRegionsEmpty() || scheduler.hasNonBlockingUpdates()) {
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
    while (true) {
        drain(Region::Active);
        if (stopping()) return false;

        if (!scheduler.empty(Region::Inactive)) {
            scheduler.promote(Region::Inactive, Region::Active);
            if (!countDelta()) return false;
            continue;
        }

        if (scheduler.hasNonBlockingUpdates()) {
            applyNonBlockingUpdates();
            if (stopping()) return false;
            if (!countDelta()) return false;
            continue;
        }

        if (!scheduler.empty(Region::Observed)) {
            drain(Region::Observed);
            if (stopping()) return false;
            continue;
        }

        if (!scheduler.empty(Region::Reactive)) {
            drain(Region::Reactive);
            if (stopping()) return false;
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

        break;
    }

    // Postponed is last and, in the M1 subset, cannot schedule anything: `$monitor` and
    // `cbReadOnlySynch` are read-only by definition.
    drain(Region::Postponed);
    return !stopping();
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
    // Applying an update can wake a process, but waking schedules into Active and never
    // appends to this list, so iterating it while applying is safe.
    for (const NonBlockingUpdate& update : scheduler.nonBlockingUpdates()) {
        if (update.partial) {
            writeSignalPart(update.signalIndex, scheduler.updateValue(update), update.lowBit);
        } else {
            writeSignal(update.signalIndex, scheduler.updateValue(update));
        }
        if (stopping()) break;
    }
    scheduler.clearNonBlocking();
}

bool Kernel::advanceTime(SimTime until) {
    const SimTime next = scheduler.nextEventTime();
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

    if (listener != nullptr) listener->onTimeAdvance(currentTime);

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

    if (listener != nullptr) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after);
}

void Kernel::writeSignalPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                             std::uint64_t lowBit) {
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

    if (listener != nullptr) listener->onValueChange(signalIndex, target);

    notifyChange(signalIndex, before, after);
}

void Kernel::scheduleNonBlocking(std::uint32_t signalIndex, eir::ConstBitRef value) {
    scheduler.appendNonBlocking(signalIndex, value, 0, false);
}

void Kernel::scheduleNonBlockingPart(std::uint32_t signalIndex, eir::ConstBitRef value,
                                     std::uint64_t lowBit) {
    scheduler.appendNonBlocking(signalIndex, value, static_cast<std::uint32_t>(lowBit), true);
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
        if (listener != nullptr) listener->onDumpFile(text);
        return;

    case eir::SysTaskKind::DumpVars:
        if (listener != nullptr) listener->onDumpVars();
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
