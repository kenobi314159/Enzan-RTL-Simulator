// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Verifier.h"

#include "common/Diagnostics.h"
#include "eir/Instruction.h"
#include "eir/Type.h"

#include <array>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

namespace {

/// Accumulates problems against one design, so that a single run reports
/// everything rather than stopping at the first defect.
class Verifier {
public:
    Verifier(const Design* enclosingDesign, DiagnosticEngine& engine)
        : design(enclosingDesign), diagnostics(engine) {}

    [[nodiscard]] bool ok() const noexcept { return problems == 0; }

    void checkModule(const ModuleDefinition& definition) {
        context = definition.name;

        for (std::size_t index = 0; index < definition.ports.size(); ++index) {
            const Port& port = definition.ports[index];
            if (port.objectIndex >= definition.objects.size()) {
                report(definition,
                       std::format("port '{}' (index {}) names object {}, but the "
                                   "module has {} objects",
                                   port.name, index, port.objectIndex, definition.objects.size()));
            }
        }

        for (const Instance& instance : definition.instances) checkInstance(definition, instance);

        for (const Process& process : definition.processes) checkProcess(definition, process);

        for (std::size_t index = 0; index < definition.subprograms.size(); ++index) {
            const Subprogram& subprogram = definition.subprograms[index];
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (definition.subprograms[earlier].name == subprogram.name) {
                    report(definition,
                           std::format("declares two subprograms called '{}'; a call names its "
                                       "callee by name, so it could not tell them apart",
                                       subprogram.name));
                    break;
                }
            }
            checkSubprogram(definition, subprogram);
        }
    }

private:
    void report(const ModuleDefinition& definition, const std::string& message) {
        ++problems;
        diagnostics.report(DiagCode::InvalidEir, definition.location,
                           std::format("module '{}': {}", definition.name, message));
    }

    void reportBody(const ModuleDefinition& definition, const Body& body,
                    const std::string& message) {
        ++problems;
        diagnostics.report(
            DiagCode::InvalidEir, body.location,
            std::format("module '{}', {} '{}': {}", definition.name, bodyNoun, body.name, message));
    }

    void checkInstance(const ModuleDefinition& definition, const Instance& instance) {
        for (const PortConnection& connection : instance.connections) {
            if (connection.connected() && connection.objectIndex >= definition.objects.size()) {
                report(definition,
                       std::format("instance '{}' connects a port to object {}, but the module "
                                   "has {} objects",
                                   instance.name, connection.objectIndex,
                                   definition.objects.size()));
            }
        }

        if (design == nullptr) return;

        const ModuleDefinition* target = design->findModule(instance.definitionName);
        if (target == nullptr) {
            // Not a verifier error on its own: analysis is per-design-unit, so a
            // reference may legitimately be unresolved until elaboration gathers
            // every library. Elaboration reports the unresolved reference.
            return;
        }

        for (const PortConnection& connection : instance.connections) {
            if (connection.portIndex >= target->ports.size()) {
                report(definition,
                       std::format("instance '{}' of '{}' binds port index {}, but that module "
                                   "has {} ports",
                                   instance.name, instance.definitionName, connection.portIndex,
                                   target->ports.size()));
            }
        }
    }

    /// Checks that an object index is in range and is declared in the given set.
    void checkObjectAccess(const ModuleDefinition& definition, const Body& body, Operand operand,
                           bool isWrite, std::string_view where) {
        if (operand.kind() != Operand::Kind::Object) {
            reportBody(definition, body, std::format("{} expects an object operand", where));
            return;
        }
        if (operand.index() >= definition.objects.size()) {
            reportBody(definition, body,
                       std::format("{} names object {}, but the module has {} objects", where,
                                   operand.index(), definition.objects.size()));
            return;
        }

        // The core invariant of section 4.2. A process that touches an object it did
        // not declare is not a pure function over a known slice of the arena, and
        // every optimisation that depends on read/write sets would then be unsound.
        const bool declared = isWrite ? writesObject(body.writeSet, operand.index())
                                      : std::ranges::binary_search(body.readSet, operand.index());
        if (!declared) {
            reportBody(definition, body,
                       std::format("{} touches object '{}' which is not in the declared {}", where,
                                   definition.objects[operand.index()].name,
                                   isWrite ? "write set" : "read set"));
        }
    }

    void checkDeclaredSet(const ModuleDefinition& definition, const Body& body,
                          const std::vector<std::uint32_t>& set, std::string_view name) {
        if (!std::ranges::is_sorted(set)) {
            reportBody(definition, body,
                       std::format("the declared {} is not sorted; set operations and "
                                   "iteration order both depend on it",
                                   name));
        }
        if (std::ranges::adjacent_find(set) != set.end()) {
            reportBody(definition, body, std::format("the declared {} contains a duplicate", name));
        }
        for (const std::uint32_t index : set) {
            if (index >= definition.objects.size()) {
                reportBody(definition, body,
                           std::format("the declared {} names object {}, but the module has "
                                       "{} objects",
                                       name, index, definition.objects.size()));
            }
        }
    }

    /// Checks that the write set is in the form `normaliseWriteSet` produces and that every
    /// extent lies inside its object.
    ///
    /// The form is checked rather than restored, because elaboration finds overlapping drivers
    /// by comparing extents (section 3.7): two touching extents of one process would be read as
    /// two drivers of that process, and an extent past the end of its object would claim bits
    /// that do not exist.
    void checkWriteSet(const ModuleDefinition& definition, const Body& body) {
        const WriteExtent* previous = nullptr;
        for (const WriteExtent& extent : body.writeSet) {
            if (extent.objectIndex >= definition.objects.size()) {
                reportBody(definition, body,
                           std::format("the declared write set names object {}, but the module "
                                       "has {} objects",
                                       extent.objectIndex, definition.objects.size()));
                return;
            }
            const Object& object = definition.objects[extent.objectIndex];

            if (extent.width == 0) {
                reportBody(
                    definition, body,
                    std::format("the declared write set has an empty extent of '{}'", object.name));
            } else if (extent.endBit() > object.type.bitWidth()) {
                reportBody(definition, body,
                           std::format("the declared write set writes bits [{}:{}] of '{}', "
                                       "which is {} bits wide",
                                       extent.endBit() - 1, extent.lowBit, object.name,
                                       object.type.bitWidth()));
            }

            if (previous != nullptr) {
                const bool ordered = previous->objectIndex < extent.objectIndex
                                     || (previous->objectIndex == extent.objectIndex
                                         && previous->endBit() < extent.lowBit);
                if (!ordered) {
                    reportBody(definition, body,
                               std::format("the declared write set is not normalised at '{}'; "
                                           "extents must be ordered, and one object's must "
                                           "neither overlap nor touch",
                                           object.name));
                }
            }
            previous = &extent;
        }
    }

    void checkSensitivityEntry(const ModuleDefinition& definition, const Body& body,
                               const SensitivityEntry& entry) {
        if (entry.objectIndex >= definition.objects.size()) {
            reportBody(definition, body,
                       std::format("is sensitive to object {}, but the module has {} objects",
                                   entry.objectIndex, definition.objects.size()));
            return;
        }
        // A process triggered by a signal depends on that signal's value, so the signal
        // belongs in the read set; otherwise activity-based scheduling and partitioning would
        // both draw the dependency graph wrongly.
        const Object& object = definition.objects[entry.objectIndex];
        if (!std::ranges::binary_search(body.readSet, entry.objectIndex)) {
            reportBody(definition, body,
                       std::format("is sensitive to '{}' but does not declare it in its read "
                                   "set",
                                   object.name));
        }
        if (entry.width != 0 && entry.lowBit + entry.width > object.type.bitWidth()) {
            reportBody(definition, body,
                       std::format("is sensitive to bits [{}:{}] of '{}', which is {} bits wide",
                                   entry.lowBit + entry.width - 1, entry.lowBit, object.name,
                                   object.type.bitWidth()));
        }
    }

    /// Returns the width of an operand that must be a register, or 0 when it is not
    /// usable, having reported why.
    std::uint32_t registerWidth(const ModuleDefinition& definition, const Body& body,
                                Operand operand, std::string_view where) {
        if (operand.kind() != Operand::Kind::Register) {
            reportBody(definition, body, std::format("{} expects a register operand", where));
            return 0;
        }
        if (operand.index() >= body.registerTypes.size()) {
            reportBody(definition, body,
                       std::format("{} uses register {}, but the {} declares {}", where,
                                   operand.index(), bodyNoun, body.registerTypes.size()));
            return 0;
        }
        return body.registerTypes[operand.index()].bitWidth();
    }

    /// Checks that a whole-object access moves a register of the object's width.
    ///
    /// A mismatch means the frontend omitted an extension or a truncation, which
    /// would silently read or write the wrong number of bits.
    void checkObjectWidth(const ModuleDefinition& definition, const Body& body,
                          Operand objectOperand, Operand registerOperand, std::string_view where) {
        if (objectOperand.kind() != Operand::Kind::Object
            || objectOperand.index() >= definition.objects.size())
            return;
        if (registerOperand.kind() != Operand::Kind::Register
            || registerOperand.index() >= body.registerTypes.size())
            return;

        const Type objectType = definition.objects[objectOperand.index()].type;
        const Type registerType = body.registerTypes[registerOperand.index()];
        if (!objectType.isBits() || !registerType.isBits()) return;

        if (objectType.bitWidth() != registerType.bitWidth()) {
            reportBody(definition, body,
                       std::format("{} moves a {}-bit register to or from object '{}', which "
                                   "is {} bits wide",
                                   where, registerType.bitWidth(),
                                   definition.objects[objectOperand.index()].name,
                                   objectType.bitWidth()));
        }
    }

    void checkProcess(const ModuleDefinition& definition, const Process& process) {
        bodyNoun = "process";
        currentProcess = &process;

        for (const SensitivityEntry& entry : process.sensitivity)
            checkSensitivityEntry(definition, process, entry);
        for (const std::vector<SensitivityEntry>& list : process.eventLists) {
            if (list.empty()) reportBody(definition, process, "has an empty event list");
            for (const SensitivityEntry& entry : list)
                checkSensitivityEntry(definition, process, entry);
        }

        checkBody(definition, process);
        currentProcess = nullptr;
    }

    void checkSubprogram(const ModuleDefinition& definition, const Subprogram& subprogram) {
        bodyNoun = "subprogram";
        currentProcess = nullptr;

        const std::size_t registers = subprogram.registerTypes.size();
        for (std::size_t index = 0; index < subprogram.parameters.size(); ++index) {
            const SubprogramParameter& parameter = subprogram.parameters[index];
            if (parameter.registerIndex >= registers) {
                reportBody(definition, subprogram,
                           std::format("parameter '{}' is register {}, but the subprogram "
                                       "declares {}",
                                       parameter.name, parameter.registerIndex, registers));
            }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (subprogram.parameters[earlier].registerIndex == parameter.registerIndex) {
                    reportBody(definition, subprogram,
                               std::format("parameters '{}' and '{}' share register {}, so a call "
                                           "would overwrite one with the other",
                                           subprogram.parameters[earlier].name, parameter.name,
                                           parameter.registerIndex));
                }
            }
        }

        if (subprogram.isFunction()) {
            if (subprogram.resultRegister >= registers) {
                reportBody(definition, subprogram,
                           std::format("its result is register {}, but the subprogram declares {}",
                                       subprogram.resultRegister, registers));
            } else if (subprogram.registerTypes[subprogram.resultRegister]
                       != subprogram.resultType) {
                reportBody(
                    definition, subprogram,
                    std::format("its result is {}, but the register holding it is {}",
                                subprogram.resultType.toString(),
                                subprogram.registerTypes[subprogram.resultRegister].toString()));
            }
        }

        checkBody(definition, subprogram);
    }

    void checkBody(const ModuleDefinition& definition, const Body& body) {
        checkDeclaredSet(definition, body, body.readSet, "read set");
        checkWriteSet(definition, body);

        // A call that writes an object writes it as a store does, so the object must be in the
        // write set, or the drivers and the scheduler would not know the process writes it.
        for (const SysTaskCall& call : body.sysCalls) {
            if (call.objectIndex != SysTaskCall::NoObject) {
                checkObjectAccess(definition, body, Operand::object(call.objectIndex), true,
                                  toString(call.kind));
            }
        }

        for (const CallSite& call : body.calls) checkCallSite(definition, body, call);

        if (body.blocks.empty()) {
            reportBody(definition, body, "has no basic blocks");
            return;
        }

        for (std::size_t blockIndex = 0; blockIndex < body.blocks.size(); ++blockIndex)
            checkBlock(definition, body, static_cast<std::uint32_t>(blockIndex));
    }

    /// Checks one call site's operands, and against its callee when the callee is in reach.
    ///
    /// A callee in another definition may be in a library this design has not gathered yet, as
    /// an instance's definition may be, so not finding it is elaboration's to report. Finding it
    /// means the call can be held to the callee's parameters, which is what keeps a call from
    /// copying a value of one width into a register of another.
    void checkCallSite(const ModuleDefinition& definition, const Body& body, const CallSite& call) {
        const std::string where = std::format("the call of '{}'", call.subprogram);

        const auto operandWidth = [&](Operand operand) -> std::optional<std::uint32_t> {
            if (operand.kind() == Operand::Kind::Register) {
                if (operand.index() < body.registerTypes.size())
                    return body.registerTypes[operand.index()].bitWidth();
                reportBody(definition, body,
                           std::format("{} uses register {}, but the {} declares {}", where,
                                       operand.index(), bodyNoun, body.registerTypes.size()));
                return std::nullopt;
            }
            if (operand.kind() == Operand::Kind::Constant) {
                if (operand.index() < body.constants.size())
                    return body.constants[operand.index()].width();
                reportBody(definition, body,
                           std::format("{} uses constant {}, but the {} has {}", where,
                                       operand.index(), bodyNoun, body.constants.size()));
                return std::nullopt;
            }
            reportBody(definition, body,
                       std::format("{} passes an argument that is neither a register nor a "
                                   "constant",
                                   where));
            return std::nullopt;
        };

        std::vector<std::optional<std::uint32_t>> argumentWidths;
        argumentWidths.reserve(call.arguments.size());
        for (const Operand argument : call.arguments)
            argumentWidths.push_back(operandWidth(argument));

        std::optional<std::uint32_t> resultWidth;
        if (call.result.valid()) {
            if (call.result.kind() != Operand::Kind::Register) {
                reportBody(definition, body,
                           std::format("{} returns its result into something other than a "
                                       "register",
                                       where));
            } else {
                resultWidth = operandWidth(call.result);
            }
        }

        const ModuleDefinition* calleeDefinition = &definition;
        if (!call.definition.empty()) {
            calleeDefinition = design == nullptr ? nullptr : design->findModule(call.definition);
            if (calleeDefinition == nullptr) return;
        }
        const std::optional<std::uint32_t> calleeIndex =
            calleeDefinition->findSubprogram(call.subprogram);
        if (!calleeIndex) {
            reportBody(definition, body,
                       std::format("calls '{}', which '{}' does not declare", call.subprogram,
                                   calleeDefinition->name));
            return;
        }
        const Subprogram& callee = calleeDefinition->subprograms[*calleeIndex];

        if (call.arguments.size() != callee.parameters.size()) {
            reportBody(definition, body,
                       std::format("{} passes {} arguments to {} parameters", where,
                                   call.arguments.size(), callee.parameters.size()));
            return;
        }
        for (std::size_t index = 0; index < callee.parameters.size(); ++index) {
            const SubprogramParameter& parameter = callee.parameters[index];
            if (parameter.mode != ParameterMode::In
                && call.arguments[index].kind() != Operand::Kind::Register) {
                reportBody(definition, body,
                           std::format("{} passes its {} parameter '{}' something other than a "
                                       "register to copy it back into",
                                       where, toString(parameter.mode), parameter.name));
            }
            if (parameter.registerIndex >= callee.registerTypes.size()) continue;
            const std::uint32_t parameterWidth =
                callee.registerTypes[parameter.registerIndex].bitWidth();
            // An argument already reported as unusable has no width to compare.
            const std::uint32_t argumentWidth = argumentWidths[index].value_or(parameterWidth);
            if (argumentWidth != parameterWidth) {
                reportBody(definition, body,
                           std::format("{} passes {} bits to parameter '{}', which is {} bits",
                                       where, argumentWidth, parameter.name, parameterWidth));
            }
        }

        if (callee.isFunction() != call.result.valid()) {
            reportBody(definition, body,
                       callee.isFunction()
                           ? std::format("{} drops the result of a function", where)
                           : std::format("{} expects a result from a procedure", where));
        } else if (const std::uint32_t width = resultWidth.value_or(callee.resultType.bitWidth());
                   width != callee.resultType.bitWidth()) {
            reportBody(definition, body,
                       std::format("{} copies a {}-bit result into a {}-bit register", where,
                                   callee.resultType.bitWidth(), width));
        }
    }

    void checkBlock(const ModuleDefinition& definition, const Body& body,
                    std::uint32_t blockIndex) {
        const BasicBlock& block = body.blocks[blockIndex];

        if (block.instructions.empty()) {
            reportBody(definition, body,
                       std::format("block {} ('{}') is empty; every block must end in a "
                                   "terminator",
                                   blockIndex, block.label));
            return;
        }

        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const Instruction& instruction = block.instructions[index];
            const bool last = index + 1 == block.instructions.size();
            const std::string where = std::format("block {} instruction {} ({})", blockIndex, index,
                                                  mnemonic(instruction.opcode));

            if (isTerminator(instruction.opcode) != last) {
                reportBody(
                    definition, body,
                    last ? std::format("{} is not a terminator but ends its block", where)
                         : std::format("{} is a terminator in the middle of its block", where));
            }

            checkInstruction(definition, body, instruction, where);
        }
    }

    void checkInstruction(const ModuleDefinition& definition, const Body& body,
                          const Instruction& instruction, const std::string& where) {
        const Opcode opcode = instruction.opcode;

        if (hasResult(opcode) != instruction.dst.valid()) {
            reportBody(definition, body,
                       std::format("{} {} a destination operand", where,
                                   hasResult(opcode) ? "is missing" : "must not have"));
        }

        const std::array<Operand, 3> sources{instruction.a, instruction.b, instruction.c};
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const bool expected = index < sourceCount(opcode);
            if (sources[index].valid() != expected) {
                reportBody(
                    definition, body,
                    std::format("{} expects {} source operands", where, sourceCount(opcode)));
                break;
            }
        }

        // Every register operand, including the destination, must exist. Checked
        // generically rather than only for the opcodes whose widths are compared,
        // because an out-of-range register index reads past the end of the register
        // file at run time — which is the class of defect a verifier exists for.
        const std::array<Operand, 4> allOperands{instruction.dst, instruction.a, instruction.b,
                                                 instruction.c};
        for (const Operand operand : allOperands) {
            if (operand.kind() == Operand::Kind::Register
                && operand.index() >= body.registerTypes.size()) {
                reportBody(definition, body,
                           std::format("{} uses register {}, but the {} declares {}", where,
                                       operand.index(), bodyNoun, body.registerTypes.size()));
            }
        }

        // Block targets must exist, or the interpreter jumps into nothing.
        for (const Operand operand : sources) {
            if (operand.kind() == Operand::Kind::Block && operand.index() >= body.blocks.size()) {
                reportBody(definition, body,
                           std::format("{} branches to block {}, but the {} has {}", where,
                                       operand.index(), bodyNoun, body.blocks.size()));
            }
            if (operand.kind() == Operand::Kind::Constant
                && operand.index() >= body.constants.size()) {
                reportBody(definition, body,
                           std::format("{} uses constant {}, but the {} has {}", where,
                                       operand.index(), bodyNoun, body.constants.size()));
            }
            if (operand.kind() == Operand::Kind::SysCall
                && operand.index() >= body.sysCalls.size()) {
                reportBody(definition, body,
                           std::format("{} calls system task descriptor {}, but the {} has {}",
                                       where, operand.index(), bodyNoun, body.sysCalls.size()));
            }
        }

        // Aliasing the destination onto a source silently corrupts the operations that
        // write as they go. Caught here rather than by a runtime assertion, so that
        // malformed EIR is a diagnostic at analysis time instead of an abort mid-run.
        if (hasResult(opcode) && instruction.dst.kind() == Operand::Kind::Register
            && !allowsAliasedDestination(opcode)) {
            for (const Operand source : sources) {
                if (source == instruction.dst) {
                    reportBody(definition, body,
                               std::format("{} uses register {} as both its destination and "
                                           "a source, which this operation does not allow",
                                           where, instruction.dst.index()));
                    break;
                }
            }
        }

        checkOpcodeSpecifics(definition, body, instruction, where);
    }

    void checkOpcodeSpecifics(const ModuleDefinition& definition, const Body& body,
                              const Instruction& instruction, const std::string& where) {
        // A subprogram runs to its return on the kernel's one call stack (section 4.1), which a
        // suspension would leave holding its frame; and its stores go straight to objects no
        // driver resolves, which a non-blocking or delayed write, scheduled through the drivers
        // of a process, is not.
        if (currentProcess == nullptr) {
            if (suspends(instruction.opcode)) {
                reportBody(definition, body,
                           std::format("{} suspends, which a subprogram may not; one that waits "
                                       "is inlined into its caller",
                                       where));
            }
            switch (instruction.opcode) {
            case Opcode::StoreNonBlocking:
            case Opcode::StorePartNonBlocking:
            case Opcode::StoreDelayed:
            case Opcode::StoreNonBlockingDelayed:
                reportBody(definition, body,
                           std::format("{} schedules a write, which a subprogram may not; its "
                                       "stores are immediate",
                                       where));
                break;
            default: break;
            }
        }

        switch (instruction.opcode) {
        case Opcode::ReadObject:
            checkObjectAccess(definition, body, instruction.a, false, where);
            checkObjectWidth(definition, body, instruction.a, instruction.dst, where);
            break;

        // A window of an object, which may be narrower than it, so only the object is checked.
        case Opcode::ReadObjectPart:
            checkObjectAccess(definition, body, instruction.a, false, where);
            break;

        case Opcode::Store:
        case Opcode::StoreNonBlocking:
            checkObjectAccess(definition, body, instruction.a, true, where);
            checkObjectWidth(definition, body, instruction.a, instruction.b, where);
            break;

        case Opcode::StoreDelayed:
        case Opcode::StoreNonBlockingDelayed:
            checkObjectAccess(definition, body, instruction.a, true, where);
            checkObjectWidth(definition, body, instruction.a, instruction.b, where);
            registerWidth(definition, body, instruction.c, where);
            break;

        // A part store deliberately moves a narrower value than the object holds,
        // so only the object itself is checked.
        case Opcode::StorePart:
        case Opcode::StorePartNonBlocking:
            checkObjectAccess(definition, body, instruction.a, true, where);
            break;

        // A system task has no result and a system function has one, so each opcode runs only
        // its own kind of call.
        case Opcode::SysTask:
        case Opcode::SysFunction:
            if (instruction.a.kind() == Operand::Kind::SysCall
                && instruction.a.index() < body.sysCalls.size()
                && isSystemFunction(body.sysCalls[instruction.a.index()].kind)
                       != (instruction.opcode == Opcode::SysFunction)) {
                reportBody(definition, body,
                           std::format("{} runs {} with the wrong opcode", where,
                                       toString(body.sysCalls[instruction.a.index()].kind)));
            }
            break;

        case Opcode::WaitEdge:
            checkObjectAccess(definition, body, instruction.a, false, where);
            break;

        case Opcode::Call:
            if (instruction.a.kind() != Operand::Kind::Call
                || instruction.a.index() >= body.calls.size()) {
                reportBody(
                    definition, body,
                    std::format("{} names a call site the {} does not have", where, bodyNoun));
            }
            break;

        case Opcode::WaitEvents:
            if (currentProcess == nullptr || instruction.a.kind() != Operand::Kind::Immediate
                || instruction.a.index() >= currentProcess->eventLists.size()) {
                reportBody(definition, body,
                           std::format("{} names an event list the process does not have", where));
            }
            break;

        // Equal-width binary operations. The frontend inserts the extensions
        // SystemVerilog's context-determined width rules call for, so a mismatch
        // here means those rules were applied wrongly — which would show up as a
        // wrong value rather than as a crash.
        case Opcode::And:
        case Opcode::Or:
        case Opcode::Xor:
        case Opcode::Xnor:
        case Opcode::Add:
        case Opcode::Subtract:
        case Opcode::Multiply:
        case Opcode::Divide:
        case Opcode::Modulo: {
            const std::uint32_t dstWidth = registerWidth(definition, body, instruction.dst, where);
            const std::uint32_t aWidth = registerWidth(definition, body, instruction.a, where);
            const std::uint32_t bWidth = registerWidth(definition, body, instruction.b, where);
            if (dstWidth != 0 && (dstWidth != aWidth || dstWidth != bWidth)) {
                reportBody(definition, body,
                           std::format("{} mixes widths {}, {} and {}; this operation "
                                       "requires all three to be equal",
                                       where, dstWidth, aWidth, bWidth));
            }
            break;
        }

        // Predicates and reductions produce exactly one bit.
        case Opcode::Equal:
        case Opcode::NotEqual:
        case Opcode::CaseEqual:
        case Opcode::CaseNotEqual:
        case Opcode::WildcardEqual:
        case Opcode::WildcardNotEqual:
        case Opcode::CaseZMatch:
        case Opcode::CaseXMatch:
        case Opcode::LessThan:
        case Opcode::LessEqual:
        case Opcode::ReduceAnd:
        case Opcode::ReduceOr:
        case Opcode::ReduceXor:
        case Opcode::LogicalAnd:
        case Opcode::LogicalOr:
        case Opcode::LogicalNot:
        case Opcode::BitSelect: {
            const std::uint32_t dstWidth = registerWidth(definition, body, instruction.dst, where);
            if (dstWidth != 0 && dstWidth != 1) {
                reportBody(definition, body,
                           std::format("{} writes a {}-bit destination, but this "
                                       "operation produces a single bit",
                                       where, dstWidth));
            }
            break;
        }

        default: break;
        }
    }

    const Design* design;
    DiagnosticEngine& diagnostics;
    std::string context;
    unsigned problems = 0;

    /// What the body being checked is called in a message: a process or a subprogram.
    std::string_view bodyNoun = "process";

    /// The process being checked, or null while checking a subprogram, which has none of a
    /// process's event lists and may neither suspend nor schedule a write.
    const Process* currentProcess = nullptr;
};

} // namespace

bool verify(const ModuleDefinition& definition, const Design* design,
            DiagnosticEngine& diagnostics) {
    Verifier verifier(design, diagnostics);
    verifier.checkModule(definition);
    return verifier.ok();
}

bool verify(const Design& design, DiagnosticEngine& diagnostics) {
    Verifier verifier(&design, diagnostics);
    for (const ModuleDefinition& definition : design.modules()) verifier.checkModule(definition);
    return verifier.ok();
}

} // namespace enzan::eir
