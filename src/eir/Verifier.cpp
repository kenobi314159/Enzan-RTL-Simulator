// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Verifier.h"

#include "common/Diagnostics.h"
#include "eir/Instruction.h"
#include "eir/Type.h"

#include <array>

#include <algorithm>
#include <format>
#include <string>

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

        checkAnnotations(definition);
        if (definition.isFunction) checkFunction(definition);
    }

private:
    void report(const ModuleDefinition& definition, const std::string& message) {
        ++problems;
        diagnostics.report(DiagCode::InvalidEir, definition.location,
                           std::format("module '{}': {}", definition.name, message));
    }

    void reportProcess(const ModuleDefinition& definition, const Process& process,
                       const std::string& message) {
        ++problems;
        diagnostics.report(
            DiagCode::InvalidEir, process.location,
            std::format("module '{}', process '{}': {}", definition.name, process.name, message));
    }

    /// Checks the shape a constant function's unit has (`ModuleDefinition::isFunction`), which
    /// is what running it relies on: arguments in, one result out, one process computing it.
    void checkFunction(const ModuleDefinition& definition) {
        if (definition.ports.empty() || definition.ports.back().direction != PortDirection::Output
            || std::ranges::any_of(
                std::span(definition.ports).first(definition.ports.size() - 1),
                [](const Port& port) { return port.direction != PortDirection::Input; })) {
            report(definition, "a constant function's ports are its arguments, as inputs, and "
                               "then one output");
        }
        if (definition.processes.size() != 1
            || definition.processes.front().kind != ProcessKind::Initial
            || !definition.instances.empty() || definition.isGeneric()) {
            report(definition, "a constant function is one initial process, with no instances "
                               "and no formals");
        }
    }

    /// Checks what makes a unit generic (DESIGN_GUIDE.md section 18, M5.1): that every
    /// annotation names an expression of the unit's table, that a register or a constant is
    /// annotated by a list as long as the one it annotates, and that a formal is defined only in
    /// terms of the formals before it, which is the order elaboration evaluates them in.
    void checkAnnotations(const ModuleDefinition& definition) {
        const std::size_t expressionCount = definition.expressions.size();
        const auto check = [&](std::uint32_t index, std::string_view what) {
            if (index != NoExpression && index >= expressionCount) {
                report(definition, std::format("{} names expression {}, but the unit has {}", what,
                                               index, expressionCount));
            }
        };

        // The highest formal each node reaches, or -1 for none. Operands come before the nodes
        // using them, so one pass in order sees every operand's answer first.
        std::vector<std::int64_t> highestFormal(expressionCount, -1);
        for (std::size_t index = 0; index < expressionCount; ++index) {
            const ParameterNode& node =
                definition.expressions.node(static_cast<std::uint32_t>(index));
            std::int64_t highest = -1;
            if (node.kind == ParameterNodeKind::Formal) {
                highest = node.a;
                if (node.a >= definition.formals.size()) {
                    report(definition, std::format("expression {} uses formal {}, but the unit has "
                                                   "{} formals",
                                                   index, node.a, definition.formals.size()));
                }
            } else if (node.kind != ParameterNodeKind::Constant) {
                for (const std::uint32_t operand : {node.a, node.b, node.c})
                    if (operand != ParameterNode::NoNode)
                        highest = std::max(highest, highestFormal[operand]);
            }
            if (node.widthNode != ParameterNode::NoNode)
                highest = std::max(highest, highestFormal[node.widthNode]);
            highestFormal[index] = highest;
        }

        for (std::size_t index = 0; index < definition.formals.size(); ++index) {
            const ParameterFormal& formal = definition.formals[index];
            for (const std::uint32_t expression : {formal.value, formal.widthExpression}) {
                check(expression, std::format("formal '{}'", formal.name));
                if (expression != NoExpression && expression < expressionCount
                    && highestFormal[expression] >= static_cast<std::int64_t>(index)) {
                    report(definition, std::format("formal '{}' is defined by a formal at or after "
                                                   "itself",
                                                   formal.name));
                }
            }
        }

        for (const Object& object : definition.objects) {
            const std::string what = std::format("object '{}'", object.name);
            check(object.type.widthExpression(), what);
            check(object.elementWidthExpression, what);
            check(object.initialValueExpression, what);
            for (const UnpackedDimension& dimension : object.unpackedDimensions) {
                check(dimension.leftExpression, what);
                check(dimension.rightExpression, what);
            }
        }

        for (const Instance& instance : definition.instances) {
            for (const ParameterActual& actual : instance.parameterActuals)
                check(actual.value, std::format("instance '{}'", instance.name));
        }

        for (const Process& process : definition.processes) {
            const std::string what = std::format("process '{}'", process.name);
            const auto checkList = [&](const std::vector<std::uint32_t>& list, std::size_t size,
                                       std::string_view of) {
                if (!list.empty() && list.size() != size) {
                    report(definition, std::format("{} annotates {} of its {} {}", what,
                                                   list.size(), size, of));
                }
                for (const std::uint32_t expression : list) check(expression, what);
            };
            for (const Type& type : process.registerTypes) check(type.widthExpression(), what);
            for (const SysTaskCall& call : process.sysCalls)
                for (const SysTaskArgument& argument : call.arguments)
                    check(argument.type.widthExpression(), what);
            checkList(process.constantExpressions, process.constants.size(), "constants");

            const auto checkEntry = [&](const SensitivityEntry& entry) {
                check(entry.lowBitExpression, what);
                check(entry.widthExpression, what);
            };
            for (const SensitivityEntry& entry : process.sensitivity) checkEntry(entry);
            for (const std::vector<SensitivityEntry>& list : process.eventLists)
                for (const SensitivityEntry& entry : list) checkEntry(entry);
            for (const WriteExtent& extent : process.writeSet) {
                check(extent.lowBitExpression, what);
                check(extent.widthExpression, what);
            }
        }
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
    void checkObjectAccess(const ModuleDefinition& definition, const Process& process,
                           Operand operand, bool isWrite, std::string_view where) {
        if (operand.kind() != Operand::Kind::Object) {
            reportProcess(definition, process, std::format("{} expects an object operand", where));
            return;
        }
        if (operand.index() >= definition.objects.size()) {
            reportProcess(definition, process,
                          std::format("{} names object {}, but the module has {} objects", where,
                                      operand.index(), definition.objects.size()));
            return;
        }

        // The core invariant of section 4.2. A process that touches an object it did
        // not declare is not a pure function over a known slice of the arena, and
        // every optimisation that depends on read/write sets would then be unsound.
        const bool declared = isWrite
                                  ? writesObject(process.writeSet, operand.index())
                                  : std::ranges::binary_search(process.readSet, operand.index());
        if (!declared) {
            reportProcess(definition, process,
                          std::format("{} touches object '{}' which is not in the declared {}",
                                      where, definition.objects[operand.index()].name,
                                      isWrite ? "write set" : "read set"));
        }
    }

    void checkDeclaredSet(const ModuleDefinition& definition, const Process& process,
                          const std::vector<std::uint32_t>& set, std::string_view name) {
        if (!std::ranges::is_sorted(set)) {
            reportProcess(definition, process,
                          std::format("the declared {} is not sorted; set operations and "
                                      "iteration order both depend on it",
                                      name));
        }
        if (std::ranges::adjacent_find(set) != set.end()) {
            reportProcess(definition, process,
                          std::format("the declared {} contains a duplicate", name));
        }
        for (const std::uint32_t index : set) {
            if (index >= definition.objects.size()) {
                reportProcess(definition, process,
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
    void checkWriteSet(const ModuleDefinition& definition, const Process& process) {
        const WriteExtent* previous = nullptr;
        for (const WriteExtent& extent : process.writeSet) {
            if (extent.objectIndex >= definition.objects.size()) {
                reportProcess(definition, process,
                              std::format("the declared write set names object {}, but the module "
                                          "has {} objects",
                                          extent.objectIndex, definition.objects.size()));
                return;
            }
            const Object& object = definition.objects[extent.objectIndex];

            if (extent.width == 0) {
                reportProcess(
                    definition, process,
                    std::format("the declared write set has an empty extent of '{}'", object.name));
            } else if (extent.endBit() > object.type.bitWidth()) {
                reportProcess(definition, process,
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
                    reportProcess(definition, process,
                                  std::format("the declared write set is not normalised at '{}'; "
                                              "extents must be ordered, and one object's must "
                                              "neither overlap nor touch",
                                              object.name));
                }
            }
            previous = &extent;
        }
    }

    void checkSensitivityEntry(const ModuleDefinition& definition, const Process& process,
                               const SensitivityEntry& entry) {
        if (entry.objectIndex >= definition.objects.size()) {
            reportProcess(definition, process,
                          std::format("is sensitive to object {}, but the module has {} objects",
                                      entry.objectIndex, definition.objects.size()));
            return;
        }
        // A process triggered by a signal depends on that signal's value, so the signal
        // belongs in the read set; otherwise activity-based scheduling and partitioning would
        // both draw the dependency graph wrongly.
        const Object& object = definition.objects[entry.objectIndex];
        if (!std::ranges::binary_search(process.readSet, entry.objectIndex)) {
            reportProcess(definition, process,
                          std::format("is sensitive to '{}' but does not declare it in its read "
                                      "set",
                                      object.name));
        }
        if (entry.width != 0 && entry.lowBit + entry.width > object.type.bitWidth()) {
            reportProcess(definition, process,
                          std::format("is sensitive to bits [{}:{}] of '{}', which is {} bits wide",
                                      entry.lowBit + entry.width - 1, entry.lowBit, object.name,
                                      object.type.bitWidth()));
        }
    }

    /// Returns the width of an operand that must be a register, or 0 when it is not
    /// usable, having reported why.
    std::uint32_t registerWidth(const ModuleDefinition& definition, const Process& process,
                                Operand operand, std::string_view where) {
        if (operand.kind() != Operand::Kind::Register) {
            reportProcess(definition, process, std::format("{} expects a register operand", where));
            return 0;
        }
        if (operand.index() >= process.registerTypes.size()) {
            reportProcess(definition, process,
                          std::format("{} uses register {}, but the process declares {}", where,
                                      operand.index(), process.registerTypes.size()));
            return 0;
        }
        return process.registerTypes[operand.index()].bitWidth();
    }

    /// Checks that a whole-object access moves a register of the object's width.
    ///
    /// A mismatch means the frontend omitted an extension or a truncation, which
    /// would silently read or write the wrong number of bits.
    void checkObjectWidth(const ModuleDefinition& definition, const Process& process,
                          Operand objectOperand, Operand registerOperand, std::string_view where) {
        if (objectOperand.kind() != Operand::Kind::Object
            || objectOperand.index() >= definition.objects.size())
            return;
        if (registerOperand.kind() != Operand::Kind::Register
            || registerOperand.index() >= process.registerTypes.size())
            return;

        const Type objectType = definition.objects[objectOperand.index()].type;
        const Type registerType = process.registerTypes[registerOperand.index()];
        if (!objectType.isBits() || !registerType.isBits()) return;

        if (objectType.bitWidth() != registerType.bitWidth()) {
            reportProcess(definition, process,
                          std::format("{} moves a {}-bit register to or from object '{}', which "
                                      "is {} bits wide",
                                      where, registerType.bitWidth(),
                                      definition.objects[objectOperand.index()].name,
                                      objectType.bitWidth()));
        }
    }

    void checkProcess(const ModuleDefinition& definition, const Process& process) {
        checkDeclaredSet(definition, process, process.readSet, "read set");
        checkWriteSet(definition, process);

        // A call that writes an object writes it as a store does, so the object must be in the
        // write set, or the drivers and the scheduler would not know the process writes it.
        for (const SysTaskCall& call : process.sysCalls) {
            if (call.objectIndex != SysTaskCall::NoObject) {
                checkObjectAccess(definition, process, Operand::object(call.objectIndex), true,
                                  toString(call.kind));
            }
        }

        if (process.blocks.empty()) {
            reportProcess(definition, process, "has no basic blocks");
            return;
        }

        for (const SensitivityEntry& entry : process.sensitivity)
            checkSensitivityEntry(definition, process, entry);
        for (const std::vector<SensitivityEntry>& list : process.eventLists) {
            if (list.empty()) reportProcess(definition, process, "has an empty event list");
            for (const SensitivityEntry& entry : list)
                checkSensitivityEntry(definition, process, entry);
        }

        for (std::size_t blockIndex = 0; blockIndex < process.blocks.size(); ++blockIndex)
            checkBlock(definition, process, static_cast<std::uint32_t>(blockIndex));
    }

    void checkBlock(const ModuleDefinition& definition, const Process& process,
                    std::uint32_t blockIndex) {
        const BasicBlock& block = process.blocks[blockIndex];

        if (block.instructions.empty()) {
            reportProcess(definition, process,
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
                reportProcess(
                    definition, process,
                    last ? std::format("{} is not a terminator but ends its block", where)
                         : std::format("{} is a terminator in the middle of its block", where));
            }

            checkInstruction(definition, process, instruction, where);
        }
    }

    void checkInstruction(const ModuleDefinition& definition, const Process& process,
                          const Instruction& instruction, const std::string& where) {
        const Opcode opcode = instruction.opcode;

        if (hasResult(opcode) != instruction.dst.valid()) {
            reportProcess(definition, process,
                          std::format("{} {} a destination operand", where,
                                      hasResult(opcode) ? "is missing" : "must not have"));
        }

        const std::array<Operand, 3> sources{instruction.a, instruction.b, instruction.c};
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const bool expected = index < sourceCount(opcode);
            if (sources[index].valid() != expected) {
                reportProcess(
                    definition, process,
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
                && operand.index() >= process.registerTypes.size()) {
                reportProcess(definition, process,
                              std::format("{} uses register {}, but the process declares {}", where,
                                          operand.index(), process.registerTypes.size()));
            }
        }

        // Block targets must exist, or the interpreter jumps into nothing.
        for (const Operand operand : sources) {
            if (operand.kind() == Operand::Kind::Block
                && operand.index() >= process.blocks.size()) {
                reportProcess(definition, process,
                              std::format("{} branches to block {}, but the process has {}", where,
                                          operand.index(), process.blocks.size()));
            }
            if (operand.kind() == Operand::Kind::Constant
                && operand.index() >= process.constants.size()) {
                reportProcess(definition, process,
                              std::format("{} uses constant {}, but the process has {}", where,
                                          operand.index(), process.constants.size()));
            }
            if (operand.kind() == Operand::Kind::SysCall
                && operand.index() >= process.sysCalls.size()) {
                reportProcess(definition, process,
                              std::format("{} calls system task descriptor {}, but the process "
                                          "has {}",
                                          where, operand.index(), process.sysCalls.size()));
            }
        }

        // Aliasing the destination onto a source silently corrupts the operations that
        // write as they go. Caught here rather than by a runtime assertion, so that
        // malformed EIR is a diagnostic at analysis time instead of an abort mid-run.
        if (hasResult(opcode) && instruction.dst.kind() == Operand::Kind::Register
            && !allowsAliasedDestination(opcode)) {
            for (const Operand source : sources) {
                if (source == instruction.dst) {
                    reportProcess(definition, process,
                                  std::format("{} uses register {} as both its destination and "
                                              "a source, which this operation does not allow",
                                              where, instruction.dst.index()));
                    break;
                }
            }
        }

        checkOpcodeSpecifics(definition, process, instruction, where);
    }

    void checkOpcodeSpecifics(const ModuleDefinition& definition, const Process& process,
                              const Instruction& instruction, const std::string& where) {
        switch (instruction.opcode) {
        case Opcode::ReadObject:
            checkObjectAccess(definition, process, instruction.a, false, where);
            checkObjectWidth(definition, process, instruction.a, instruction.dst, where);
            break;

        // A window of an object, which may be narrower than it, so only the object is checked.
        case Opcode::ReadObjectPart:
            checkObjectAccess(definition, process, instruction.a, false, where);
            break;

        case Opcode::Store:
        case Opcode::StoreNonBlocking:
            checkObjectAccess(definition, process, instruction.a, true, where);
            checkObjectWidth(definition, process, instruction.a, instruction.b, where);
            break;

        case Opcode::StoreDelayed:
        case Opcode::StoreNonBlockingDelayed:
            checkObjectAccess(definition, process, instruction.a, true, where);
            checkObjectWidth(definition, process, instruction.a, instruction.b, where);
            registerWidth(definition, process, instruction.c, where);
            break;

        // A part store deliberately moves a narrower value than the object holds,
        // so only the object itself is checked.
        case Opcode::StorePart:
        case Opcode::StorePartNonBlocking:
            checkObjectAccess(definition, process, instruction.a, true, where);
            break;

        // A system task has no result and a system function has one, so each opcode runs only
        // its own kind of call.
        case Opcode::SysTask:
        case Opcode::SysFunction:
            if (instruction.a.kind() == Operand::Kind::SysCall
                && instruction.a.index() < process.sysCalls.size()
                && isSystemFunction(process.sysCalls[instruction.a.index()].kind)
                       != (instruction.opcode == Opcode::SysFunction)) {
                reportProcess(definition, process,
                              std::format("{} runs {} with the wrong opcode", where,
                                          toString(process.sysCalls[instruction.a.index()].kind)));
            }
            break;

        case Opcode::WaitEdge:
            checkObjectAccess(definition, process, instruction.a, false, where);
            break;

        case Opcode::WaitEvents:
            if (instruction.a.kind() != Operand::Kind::Immediate
                || instruction.a.index() >= process.eventLists.size()) {
                reportProcess(
                    definition, process,
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
            const std::uint32_t dstWidth =
                registerWidth(definition, process, instruction.dst, where);
            const std::uint32_t aWidth = registerWidth(definition, process, instruction.a, where);
            const std::uint32_t bWidth = registerWidth(definition, process, instruction.b, where);
            if (dstWidth != 0 && (dstWidth != aWidth || dstWidth != bWidth)) {
                reportProcess(definition, process,
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
            const std::uint32_t dstWidth =
                registerWidth(definition, process, instruction.dst, where);
            if (dstWidth != 0 && dstWidth != 1) {
                reportProcess(definition, process,
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
