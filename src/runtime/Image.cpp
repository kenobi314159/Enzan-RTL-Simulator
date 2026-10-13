// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/Image.h"

#include "common/Assert.h"
#include "eir/BitOps.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include <algorithm>
#include <format>
#include <vector>

namespace enzan::runtime {

Image::Image(const eir::Design& analysedDesign, const elab::ElaboratedDesign& elaboratedDesign,
             const codegen::CompiledDesign& compiledDesign)
    : design(&analysedDesign), elaborated(&elaboratedDesign), compiled(&compiledDesign) {
    arenaSize = elaboratedDesign.arenaWords();
    buildSignals();
    buildInstanceTables();
    buildProcesses();
    buildSubprograms();
    measureCallStack();
}

void Image::buildSignals() {
    signalList.reserve(elaborated->slots().size());
    for (const elab::Slot& slot : elaborated->slots()) {
        signalList.push_back(Signal{.wordOffset = slot.wordOffset,
                                    .bitWidth = slot.type.bitWidth(),
                                    .path = slot.path,
                                    .encoding = slot.encoding,
                                    .subscribers = {}});
    }
}

void Image::buildInstanceTables() {
    instanceObjectOffsets.resize(elaborated->instances().size());
    instanceObjectSignals.resize(elaborated->instances().size());

    for (std::size_t instanceIndex = 0; instanceIndex < elaborated->instances().size();
         ++instanceIndex) {
        const elab::Instance& instance = elaborated->instances()[instanceIndex];
        std::vector<std::uint32_t>& offsets = instanceObjectOffsets[instanceIndex];
        std::vector<std::uint32_t>& signals = instanceObjectSignals[instanceIndex];

        offsets.reserve(instance.objectSlots.size());
        signals.reserve(instance.objectSlots.size());
        for (const std::uint32_t slotIndex : instance.objectSlots) {
            offsets.push_back(elaborated->slots()[slotIndex].wordOffset);
            signals.push_back(slotIndex);
        }
    }
}

void Image::buildProcesses() {
    processList.reserve(elaborated->processes().size());
    processWriteRoutes.resize(elaborated->processes().size());

    // Drivers are ordered by process, then object, so one pass hands each process its runs.
    const std::span<const elab::Driver> drivers = elaborated->drivers();
    std::size_t nextDriver = 0;

    for (const elab::ProcessInstance& processInstance : elaborated->processes()) {
        const elab::Instance& instance = elaborated->instances()[processInstance.instanceIndex];
        const std::uint32_t bodyIndex =
            compiled->bodyIndex(instance.definitionIndex, processInstance.processIndex);
        ENZAN_ALWAYS_ASSERT(bodyIndex != codegen::CompiledDesign::NotCompiled,
                            "every elaborated process must have a compiled body");

        const codegen::CompiledBody& body = compiled->body(bodyIndex);

        ProcessState state;
        state.bodyIndex = bodyIndex;
        state.instanceIndex = processInstance.instanceIndex;
        state.frameOffset = frameSize;
        state.objectOffsets = instanceObjectOffsets[processInstance.instanceIndex];
        state.objectSignals = instanceObjectSignals[processInstance.instanceIndex];
        state.calls = processInstance.calls;
        state.name = std::format("{} {}", instance.path, body.name);
        frameSize += body.frameWords;

        const auto processIndex = static_cast<std::uint32_t>(processList.size());

        std::vector<WriteRoute>& routes = processWriteRoutes[processIndex];
        for (const std::uint32_t objectIndex : eir::writtenObjects(body.writeSet)) {
            WriteRoute route{.firstDriver = static_cast<std::uint32_t>(nextDriver),
                             .driverCount = 0};
            while (nextDriver < drivers.size() && drivers[nextDriver].process == processIndex
                   && drivers[nextDriver].objectIndex == objectIndex) {
                ++route.driverCount;
                ++nextDriver;
            }
            routes.push_back(route);
        }
        ENZAN_ALWAYS_ASSERT(nextDriver == drivers.size()
                                || drivers[nextDriver].process != processIndex,
                            "every driver of a process is reached through one of its routes");
        state.writeRoutes = routes;

        // Static sensitivity. An explicit list is used as written; a process whose
        // triggering is implicit — `always_comb`, `always_latch`, a continuous
        // assignment — is subscribed to everything it reads, which is what the language
        // means by an inferred sensitivity list. Not to a subroutine's static variable, which
        // every caller writes: callers watching it would wake one another for ever.
        const bool inferred = body.sensitivity.empty()
                              && (body.kind == eir::ProcessKind::AlwaysComb
                                  || body.kind == eir::ProcessKind::AlwaysLatch
                                  || body.kind == eir::ProcessKind::ContinuousAssign);

        if (inferred) {
            const std::vector<eir::Object>& objects =
                design->module(instance.definitionIndex).objects;
            for (const std::uint32_t objectIndex : body.readSet) {
                if (objects[objectIndex].subroutineStatic) continue;
                signalList[state.objectSignals[objectIndex]].subscribers.push_back(
                    Signal::Subscriber{.processIndex = processIndex,
                                       .edge = eir::EdgeKind::AnyChange});
            }
        } else {
            for (const eir::SensitivityEntry& entry : body.sensitivity) {
                Signal& signal = signalList[state.objectSignals[entry.objectIndex]];
                signal.subscribers.push_back(Signal::Subscriber{.processIndex = processIndex,
                                                                .edge = entry.edge,
                                                                .lowBit = entry.lowBit,
                                                                .width = entry.width});
                if (entry.width != 0) signal.windowed = true;
            }
        }
        for (const std::vector<eir::SensitivityEntry>& list : body.eventLists) {
            for (const eir::SensitivityEntry& entry : list)
                if (entry.width != 0)
                    signalList[state.objectSignals[entry.objectIndex]].windowed = true;
        }

        processList.push_back(std::move(state));
    }
}

void Image::buildSubprograms() {
    const std::span<const elab::SubprogramInstance> bound = elaborated->subprograms();
    subprogramList.reserve(bound.size());
    subprogramWriteRoutes.resize(bound.size());

    for (std::size_t index = 0; index < bound.size(); ++index) {
        const elab::SubprogramInstance& subprogramInstance = bound[index];
        const std::uint32_t bodyIndex = compiled->subprogramBodyIndex(
            subprogramInstance.definitionIndex, subprogramInstance.subprogramIndex);
        ENZAN_ALWAYS_ASSERT(bodyIndex != codegen::CompiledDesign::NotCompiled,
                            "every bound subprogram must have a compiled body");

        BoundSubprogram subprogram;
        subprogram.bodyIndex = bodyIndex;
        subprogram.calls = subprogramInstance.calls;
        if (subprogramInstance.instanceIndex != elab::SubprogramInstance::NoInstance) {
            subprogram.objectOffsets = instanceObjectOffsets[subprogramInstance.instanceIndex];
            subprogram.objectSignals = instanceObjectSignals[subprogramInstance.instanceIndex];
        }

        std::vector<WriteRoute>& routes = subprogramWriteRoutes[index];
        routes.assign(eir::writtenObjects(compiled->body(bodyIndex).writeSet).size(),
                      WriteRoute{.firstDriver = 0, .driverCount = 0});
        subprogram.writeRoutes = routes;

        subprogramList.push_back(subprogram);
    }
}

void Image::measureCallStack() {
    // Depth-first over the bound calls, memoising each subprogram's deepest chain. A call that
    // reaches a subprogram still on the path is recursion, which no depth bounds.
    constexpr std::uint8_t Unvisited = 0;
    constexpr std::uint8_t OnPath = 1;
    constexpr std::uint8_t Done = 2;
    std::vector<std::uint8_t> state(subprogramList.size(), Unvisited);
    std::vector<std::uint32_t> depth(subprogramList.size(), 0);
    std::vector<std::uint32_t> words(subprogramList.size(), 0);

    struct Step {
        std::uint32_t subprogram;
        std::size_t nextCall;
    };

    std::vector<Step> path;

    for (std::uint32_t root = 0; root < subprogramList.size(); ++root) {
        if (state[root] != Unvisited) continue;
        state[root] = OnPath;
        path.push_back(Step{.subprogram = root, .nextCall = 0});

        while (!path.empty()) {
            Step& step = path.back();
            const BoundSubprogram& subprogram = subprogramList[step.subprogram];
            if (step.nextCall < subprogram.calls.size()) {
                const std::uint32_t callee = subprogram.calls[step.nextCall++];
                if (state[callee] == OnPath) stackShape.recursive = true;
                if (state[callee] != Unvisited) continue;
                state[callee] = OnPath;
                path.push_back(Step{.subprogram = callee, .nextCall = 0});
                continue;
            }

            const std::uint32_t frame = compiled->body(subprogram.bodyIndex).frameWords;
            stackShape.largestFrame = std::max(stackShape.largestFrame, frame);
            std::uint32_t deepest = 0;
            std::uint32_t heaviest = 0;
            for (const std::uint32_t callee : subprogram.calls) {
                deepest = std::max(deepest, depth[callee]);
                heaviest = std::max(heaviest, words[callee]);
            }
            depth[step.subprogram] = deepest + 1;
            words[step.subprogram] = heaviest + frame;
            state[step.subprogram] = Done;
            path.pop_back();
        }
    }

    for (const ProcessState& process : processList) {
        for (const std::uint32_t callee : process.calls) {
            stackShape.depth = std::max(stackShape.depth, depth[callee]);
            stackShape.words = std::max(stackShape.words, words[callee]);
        }
    }

    // The kernel calls a VHDL resolution function itself, while no process's call is in progress
    // (DESIGN_GUIDE.md section 3.7), so each is a chain of its own.
    for (const elab::ResolutionCall& call : elaborated->resolutionCalls()) {
        if (call.subprogramInstance >= subprogramList.size()) continue;
        stackShape.depth = std::max(stackShape.depth, depth[call.subprogramInstance]);
        stackShape.words = std::max(stackShape.words, words[call.subprogramInstance]);
    }
}

void Image::initializeState(Arena& arena) const {
    // Initialisation is per **slot**, not per object, and an explicit initialiser always
    // wins over a type default.
    //
    // Elaboration collapses a connected port onto the parent's slot, so one slot is reached
    // through several objects. A single pass over objects would let whichever instance came
    // last overwrite the others — and since a child's port declaration carries no
    // initialiser, that meant a parent's `logic rst_n = 1'b0;` was quietly replaced by X.
    // The design then simulated and produced a plausible wrong answer, which is the failure
    // mode section 1.2 ranks above all others.
    std::vector<bool> initialized(signalList.size(), false);

    const auto visit = [&](bool wantExplicit) {
        for (std::size_t instanceIndex = 0; instanceIndex < elaborated->instances().size();
             ++instanceIndex) {
            const elab::Instance& instance = elaborated->instances()[instanceIndex];
            const eir::ModuleDefinition& definition = design->module(instance.definitionIndex);

            for (std::uint32_t objectIndex = 0; objectIndex < definition.objects.size();
                 ++objectIndex) {
                const eir::Object& object = definition.objects[objectIndex];
                if (!object.type.isBits()) continue;
                if (object.initialValue.has_value() != wantExplicit) continue;

                const std::uint32_t slotIndex = instance.objectSlots[objectIndex];
                if (initialized[slotIndex]) continue;
                initialized[slotIndex] = true;

                const eir::BitRef target = arena.at(
                    instanceObjectOffsets[instanceIndex][objectIndex], object.type.bitWidth());

                if (wantExplicit) {
                    eir::bitops::extend(target, object.initialValue->ref(), object.type.isSigned());
                    continue;
                }

                // No declared initialiser: a net's bits hold what an undriven bit of its kind
                // holds, `z` or its pull, until a driver says otherwise (section 3.7). A
                // variable is X when four-state and zero when two-state (section 3.9); one
                // that defaulted to zero would hide exactly the bugs X exists to reveal. The
                // slot's kind decides, not the object's, since a collapsed port's object is
                // only another name for its parent's declaration.
                const elab::Slot& slot = elaborated->slots()[slotIndex];
                if (slot.kind == eir::ObjectKind::Net) {
                    target.fill(eir::undrivenValue(slot.netKind));
                    for (const eir::WriteExtent& driven : slot.drivenBits) {
                        eir::bitops::assignPartAt(target, eir::BitVector(driven.width).ref(),
                                                  driven.lowBit);
                    }
                } else {
                    target.fill(object.type.isFourState() ? eir::Logic::Unknown : eir::Logic::Zero);
                }
            }
        }
    };

    // Explicit initialisers first, so that a collapsed slot takes the declared value
    // whichever side of the port connection declared it.
    visit(true);
    visit(false);

    // A driver that has not run yet has no value the design gave it, so it starts at X, as the
    // net's driven bits do. A VHDL driver starts at its signal's initial value instead (IEEE 1076
    // section 14.7.2), which its resolution function is given until the driver first runs.
    for (const elab::Driver& driver : elaborated->drivers()) {
        const elab::Slot& slot = elaborated->slots()[driver.slot];
        const eir::BitRef value = arena.at(driver.wordOffset, driver.width);
        if (slot.resolution.resolved()) {
            eir::bitops::partSelectAt(value, arena.at(slot.wordOffset, slot.type.bitWidth()),
                                      driver.lowBit);
        } else {
            value.fill(eir::Logic::Unknown);
        }
    }
}

std::uint32_t Image::findSignal(std::string_view path) const {
    const std::optional<std::uint32_t> slot = elaborated->findSlot(path);
    return slot.has_value() ? *slot : NoSignal;
}

} // namespace enzan::runtime
