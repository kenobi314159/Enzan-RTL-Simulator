// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Boundary.h"

#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace enzan::elab {

namespace {

using eir::Encoding;
using eir::Instruction;
using eir::Opcode;
using eir::Operand;

/// A run of a VHDL value's bits that mean one thing: its lowest bit, its width and what it holds.
struct Leaf {
    std::uint32_t offset = 0;
    std::uint32_t width = 0;
    Encoding encoding = Encoding::Bits;

    /// How many bits it is on the SystemVerilog side.
    [[nodiscard]] std::uint32_t shownWidth() const noexcept {
        return eir::presentedWidth(encoding, width);
    }
};

/// Appends the leaves of a value of `typeIndex` whose lowest bit is `base`; false, with `refusal`
/// saying why, when one has no SystemVerilog counterpart.
bool appendLeaves(const eir::ModuleDefinition& definition, std::uint32_t typeIndex,
                  std::uint32_t base, std::vector<Leaf>& leaves, std::string& refusal) {
    if (typeIndex >= definition.vhdlTypes.size()) {
        refusal = "its VHDL type is not known";
        return false;
    }
    const eir::VhdlType& type = definition.vhdlTypes[typeIndex];
    switch (type.kind) {
    case eir::VhdlType::Kind::Enumeration:
        if (type.encoding != Encoding::VhdlLogic && type.encoding != Encoding::VhdlBit) {
            refusal = std::format("the enumeration {} has no SystemVerilog counterpart", type.name);
            return false;
        }
        leaves.push_back({.offset = base, .width = type.width, .encoding = type.encoding});
        return true;
    case eir::VhdlType::Kind::Integer:
        leaves.push_back({.offset = base, .width = type.width, .encoding = Encoding::Bits});
        return true;
    case eir::VhdlType::Kind::Physical:
        refusal = std::format("the physical type {} has no SystemVerilog counterpart", type.name);
        return false;
    case eir::VhdlType::Kind::Floating:
        refusal = "a real has no type in the SystemVerilog frontend yet";
        return false;
    case eir::VhdlType::Kind::Array: {
        if (type.element >= definition.vhdlTypes.size()) {
            refusal = "its element type is not known";
            return false;
        }
        const std::uint32_t elementWidth = definition.vhdlTypes[type.element].width;
        if (elementWidth == 0 || type.width == 0) {
            refusal = "a null array holds nothing to convert";
            return false;
        }
        for (std::uint32_t element = 0; element < type.width / elementWidth; ++element) {
            if (!appendLeaves(definition, type.element, base + (element * elementWidth), leaves,
                              refusal))
                return false;
        }
        return true;
    }
    case eir::VhdlType::Kind::Record:
        for (const eir::VhdlType::Field& field : type.fields) {
            if (!appendLeaves(definition, field.type, base + field.offset, leaves, refusal))
                return false;
        }
        return true;
    }
    refusal = "its VHDL type is not one Enzan knows";
    return false;
}

/// The leaves of `object`, a VHDL one, the highest first, neighbours that mean one thing merged;
/// nothing, with `refusal` saying why, when it has a part no rule converts.
std::optional<std::vector<Leaf>> leavesOf(const eir::ModuleDefinition& definition,
                                          const eir::Object& object, std::string& refusal) {
    std::vector<Leaf> leaves;
    if (object.vhdlType == eir::VhdlType::None) {
        // With no type to say otherwise, the object's encoding is what all of it holds.
        if (object.encoding == Encoding::Real) {
            refusal = "a real has no type in the SystemVerilog frontend yet";
            return std::nullopt;
        }
        leaves.push_back(
            {.offset = 0, .width = object.type.bitWidth(), .encoding = object.encoding});
    } else if (!appendLeaves(definition, object.vhdlType, 0, leaves, refusal)) {
        return std::nullopt;
    }

    // The left element and the first field are the highest bits on both sides.
    std::ranges::sort(
        leaves, [](const Leaf& left, const Leaf& right) { return left.offset > right.offset; });
    std::vector<Leaf> merged;
    for (const Leaf& leaf : leaves) {
        if (!merged.empty() && merged.back().encoding == leaf.encoding
            && merged.back().offset == leaf.offset + leaf.width) {
            merged.back().offset = leaf.offset;
            merged.back().width += leaf.width;
            continue;
        }
        merged.push_back(leaf);
    }
    return merged;
}

/// What `object` holds before anything runs: its initial value, or else X when it is four-state and
/// 0 when it is not.
eir::BitVector startOf(const eir::Object& object) {
    const std::uint32_t width = object.type.bitWidth();
    if (object.initialValue && object.initialValue->width() == width) return *object.initialValue;
    return eir::BitVector::filled(width, object.type.isFourState() ? eir::Logic::Unknown
                                                                   : eir::Logic::Zero);
}

/// `value`, of one side's representation, as `convert` converts it into `target`'s, the other's.
eir::BitVector converted(const eir::BitVector& value, const std::vector<Leaf>& leaves, bool toShown,
                         eir::Type target) {
    eir::BitVector result = eir::BitVector::filled(target.bitWidth(), eir::Logic::Zero);
    std::uint32_t shown = 0;
    for (const Leaf& leaf : std::views::reverse(leaves)) {
        eir::BitVector part(toShown ? leaf.width : leaf.shownWidth());
        eir::bitops::partSelectAt(part.mutableRef(), value.ref(), toShown ? leaf.offset : shown);
        eir::BitVector into(toShown ? leaf.shownWidth() : leaf.width);
        if (leaf.encoding == Encoding::Bits) {
            into = part;
        } else if (toShown) {
            eir::present(leaf.encoding, part.ref(), into.mutableRef());
        } else {
            eir::encode(leaf.encoding, part.ref(), into.mutableRef());
        }
        eir::bitops::assignPartAt(result.mutableRef(), into.ref(), toShown ? shown : leaf.offset);
        shown += leaf.shownWidth();
    }
    if (!target.isFourState()) {
        for (std::uint32_t bit = 0; bit < result.width(); ++bit)
            result.set(bit,
                       result.get(bit) == eir::Logic::One ? eir::Logic::One : eir::Logic::Zero);
    }
    return result;
}

/// Writes one process's body.
class Builder {
public:
    explicit Builder(eir::Process& target) : process(&target) {}

    std::uint32_t reg(eir::Type type) {
        process->registerTypes.push_back(type);
        return static_cast<std::uint32_t>(process->registerTypes.size() - 1);
    }

    /// A register holding `value`, of `type`.
    std::uint32_t constant(const eir::BitVector& value, eir::Type type) {
        process->constants.push_back(value);
        const std::uint32_t result = reg(type);
        emit(Instruction::make(
            Opcode::LoadConstant, Operand::reg(result),
            Operand::constant(static_cast<std::uint32_t>(process->constants.size() - 1))));
        return result;
    }

    /// A register holding a bit offset.
    std::uint32_t offset(std::uint32_t value) {
        return constant(eir::BitVector::fromUInt64(value, 32), eir::Type::bits(32, false, false));
    }

    std::uint32_t block(std::string label) {
        process->blocks.push_back(eir::BasicBlock{.label = std::move(label), .instructions = {}});
        current = static_cast<std::uint32_t>(process->blocks.size() - 1);
        return current;
    }

    void emit(Instruction instruction) {
        process->blocks[current].instructions.push_back(instruction);
    }

private:
    eir::Process* process;
    std::uint32_t current = 0;
};

/// Converts `value`, a register of one side's representation, into a new register of `target`,
/// the other side's: SystemVerilog's bits when `toShown`, the VHDL side's codes otherwise.
std::uint32_t convert(Builder& body, std::uint32_t value, const std::vector<Leaf>& leaves,
                      bool toShown, eir::Type target) {
    // Each leaf's own conversion, from a register of its bits on the side converted from.
    const auto one = [&](const Leaf& leaf, std::uint32_t from) {
        const std::uint32_t into =
            body.reg(eir::Type::bits(toShown ? leaf.shownWidth() : leaf.width, false, true));
        if (leaf.encoding == Encoding::Bits) {
            body.emit(Instruction::make(Opcode::Move, Operand::reg(into), Operand::reg(from)));
        } else {
            body.emit(Instruction::make(
                toShown ? Opcode::Present : Opcode::Encode, Operand::reg(into), Operand::reg(from),
                Operand::immediate(static_cast<std::uint32_t>(leaf.encoding))));
        }
        return into;
    };

    const eir::Type wide = eir::Type::bits(target.bitWidth(), false, true);
    std::uint32_t result = 0;
    if (leaves.size() == 1) {
        result = one(leaves.front(), value);
    } else {
        // Leaf by leaf, from the lowest up, each taken out of the value and spliced into place.
        result = body.constant(eir::BitVector::filled(target.bitWidth(), eir::Logic::Zero), wide);
        std::uint32_t shown = 0;
        for (const Leaf& leaf : std::views::reverse(leaves)) {
            const std::uint32_t fromOffset = toShown ? leaf.offset : shown;
            const std::uint32_t intoOffset = toShown ? shown : leaf.offset;
            const std::uint32_t part =
                body.reg(eir::Type::bits(toShown ? leaf.width : leaf.shownWidth(), false, true));
            body.emit(Instruction::make(Opcode::PartSelect, Operand::reg(part), Operand::reg(value),
                                        Operand::reg(body.offset(fromOffset))));
            const std::uint32_t converted = one(leaf, part);
            body.emit(Instruction::make(Opcode::SplicePart, Operand::reg(result),
                                        Operand::reg(converted),
                                        Operand::reg(body.offset(intoOffset))));
            shown += leaf.shownWidth();
        }
    }

    // Into a two-state object, an unknown bit is 0, as assigning one makes it.
    if (!target.isFourState()) {
        const std::uint32_t coerced = body.reg(target);
        body.emit(
            Instruction::make(Opcode::CoerceTwoState, Operand::reg(coerced), Operand::reg(result)));
        return coerced;
    }
    return result;
}

/// The parts of one conversion process: what it reads and writes, and how it converts.
struct Conversion {
    std::string name;
    SourceLocation location;
    std::uint32_t from = 0;
    eir::Type fromType;
    std::uint32_t to = 0;
    eir::Type toType;
    bool toShown = false;
};

/// A process that converts `conversion.from` into `conversion.to` whenever it changes, and once at
/// time zero, in one delta cycle: with a transaction of its own driver, as a VHDL signal
/// assignment is, when `drives`, or else with a non-blocking store.
eir::Process converter(const Conversion& conversion, const std::vector<Leaf>& leaves, bool drives) {
    eir::Process process;
    process.kind = eir::ProcessKind::Always;
    process.name = conversion.name;
    process.location = conversion.location;
    process.drivesSignals = drives;

    Builder body(process);
    body.block("entry");
    const std::uint32_t loop = 1;
    body.emit(Instruction::make(Opcode::Jump, {}, Operand::block(loop)));
    body.block("convert");

    const std::uint32_t read = body.reg(conversion.fromType);
    body.emit(Instruction::make(Opcode::ReadObject, Operand::reg(read),
                                Operand::object(conversion.from)));
    const std::uint32_t converted =
        convert(body, read, leaves, conversion.toShown, conversion.toType);
    if (drives) {
        const std::uint32_t now =
            body.constant(eir::BitVector::fromUInt64(0, 64), eir::Type::bits(64, true, false));
        body.emit(Instruction::make(Opcode::Drive, {}, Operand::object(conversion.to),
                                    Operand::reg(converted), Operand::reg(now)));
    } else {
        body.emit(Instruction::make(Opcode::StoreNonBlocking, {}, Operand::object(conversion.to),
                                    Operand::reg(converted)));
    }

    process.eventLists.push_back({eir::SensitivityEntry{.objectIndex = conversion.from}});
    body.emit(
        Instruction::make(Opcode::WaitEvents, {}, Operand::immediate(0), Operand::block(loop)));

    process.readSet = {conversion.from};
    process.writeSet = {eir::WriteExtent::whole(conversion.to, conversion.toType.bitWidth())};
    return process;
}

/// A continuous assignment of `from` to `to`, a driver of `to` among any others it has.
eir::Process assignment(std::string name, SourceLocation location, std::uint32_t from,
                        eir::Type type, std::uint32_t to) {
    eir::Process process;
    process.kind = eir::ProcessKind::ContinuousAssign;
    process.name = std::move(name);
    process.location = location;
    process.sensitivity.push_back(eir::SensitivityEntry{.objectIndex = from});

    Builder body(process);
    body.block("entry");
    const std::uint32_t read = body.reg(type);
    body.emit(Instruction::make(Opcode::ReadObject, Operand::reg(read), Operand::object(from)));
    body.emit(Instruction::make(Opcode::Store, {}, Operand::object(to), Operand::reg(read)));
    body.emit(Instruction::make(Opcode::Return));

    process.readSet = {from};
    process.writeSet = {eir::WriteExtent::whole(to, type.bitWidth())};
    return process;
}

/// `object` as one of the instantiating definition's, called `name`: the port's representation
/// without what refers to its own definition's tables, and written by `writer`s of its own.
eir::Object standIn(const eir::Object& object, std::string name, SourceLocation location) {
    eir::Object result = object;
    result.name = std::move(name);
    result.location = location;
    result.vhdlType = eir::VhdlType::None;
    result.enumIndex = eir::Object::NoEnum;
    result.hierarchicalPath.clear();
    return result;
}

/// Connects one port of `instance` through conversions, adding to `parent` what that takes.
bool bridge(eir::ModuleDefinition& parent, const eir::Instance& instance,
            const eir::ModuleDefinition& child, const eir::Port& port,
            eir::PortConnection& connection, const std::string& base,
            DiagnosticEngine& diagnostics) {
    const eir::Object inner = child.objects[port.objectIndex];
    const std::uint32_t outerRead = connection.objectIndex;
    const std::uint32_t outerDrive = connection.drivenObject();
    const eir::Object outer = parent.objects[outerRead];
    const bool vhdlInside = child.language == eir::Language::Vhdl;
    const eir::Object& vhdlObject = vhdlInside ? inner : outer;
    const eir::Object& svObject = vhdlInside ? outer : inner;

    // What converts is what the VHDL side's type is made of, and the shapes have to agree.
    std::string refusal;
    const std::optional<std::vector<Leaf>> leaves =
        leavesOf(vhdlInside ? child : parent, vhdlObject, refusal);
    if (leaves) {
        std::uint32_t coded = 0;
        std::uint32_t shown = 0;
        for (const Leaf& leaf : *leaves) {
            coded += leaf.width;
            shown += leaf.shownWidth();
        }
        if (coded != vhdlObject.type.bitWidth()) {
            refusal = "its VHDL type does not cover its bits";
        } else if (shown != svObject.type.bitWidth()) {
            refusal = std::format("its VHDL side is {} bits in SystemVerilog and its "
                                  "SystemVerilog side {}",
                                  shown, svObject.type.bitWidth());
        }
    }
    if (!refusal.empty()) {
        diagnostics.report(DiagCode::UnconvertiblePort, instance.location, port.name, instance.name,
                           refusal);
        return false;
    }

    const eir::Type innerType = inner.type;
    const eir::Type outerType = outer.type;
    const auto add = [&parent](eir::Object object) { return parent.addObject(std::move(object)); };
    const auto addProcess = [&parent](eir::Process process) {
        parent.processes.push_back(std::move(process));
    };

    // Into the inner object from the outer, which the port reads: by a transaction into VHDL, or
    // by a non-blocking store into SystemVerilog, where nothing else writes it.
    const auto inward = [&](std::uint32_t into) {
        addProcess(converter(Conversion{.name = base + "$in",
                                        .location = instance.location,
                                        .from = outerRead,
                                        .fromType = outerType,
                                        .to = into,
                                        .toType = innerType,
                                        .toShown = !vhdlInside},
                             *leaves, vhdlInside));
    };

    // Out of the inner object the port's drivers drive, into the outer: by a transaction into
    // VHDL, the outer signal's driver; or into SystemVerilog through the non-blocking region into
    // an object of its own, which a continuous assignment drives the outer object from, so that a
    // net with other drivers resolves them.
    const auto outward = [&](std::uint32_t from) {
        if (vhdlInside) {
            eir::Object shownObject = standIn(outer, base + "$shown", instance.location);
            shownObject.kind = eir::ObjectKind::Variable;
            shownObject.initialValue =
                converted(startOf(parent.objects[from]), *leaves, true, outerType);
            shownObject.resolution = {};
            const std::uint32_t shownIndex = add(std::move(shownObject));
            addProcess(converter(Conversion{.name = base + "$out",
                                            .location = instance.location,
                                            .from = from,
                                            .fromType = innerType,
                                            .to = shownIndex,
                                            .toType = outerType,
                                            .toShown = true},
                                 *leaves, false));
            addProcess(
                assignment(base + "$assign", instance.location, shownIndex, outerType, outerDrive));
            return;
        }
        addProcess(converter(Conversion{.name = base + "$out",
                                        .location = instance.location,
                                        .from = from,
                                        .fromType = innerType,
                                        .to = outerDrive,
                                        .toType = outerType,
                                        .toShown = false},
                             *leaves, true));
    };

    // The port's own representation, which the port reads: written by the inward conversion
    // alone, so one VHDL driver or a SystemVerilog variable. It starts as the outer object does,
    // converted, as a VHDL port takes its actual's initial value, so that the first conversion is
    // no event: an edge at time zero from a value nothing ever held.
    const auto effective = [&](std::string name) {
        eir::Object object = standIn(inner, std::move(name), instance.location);
        object.resolution = {};
        if (!vhdlInside) object.kind = eir::ObjectKind::Variable;
        object.initialValue = converted(startOf(outer), *leaves, !vhdlInside, innerType);
        return add(std::move(object));
    };

    switch (port.direction) {
    case eir::PortDirection::Input: {
        connection.objectIndex = effective(base);
        connection.driveObjectIndex = eir::PortConnection::Unconnected;
        inward(connection.objectIndex);
        return true;
    }
    case eir::PortDirection::Output: {
        // What the instance's drivers drive, resolved as the port's own type resolves them.
        connection.objectIndex = add(standIn(inner, base, instance.location));
        connection.driveObjectIndex = eir::PortConnection::Unconnected;
        outward(connection.objectIndex);
        return true;
    }
    case eir::PortDirection::InOut: {
        // Read as the outer signal brought back, driven as one source of it.
        const std::uint32_t read = effective(base);
        const std::uint32_t driven = add(standIn(inner, base + "$driving", instance.location));
        connection.objectIndex = read;
        connection.driveObjectIndex = driven;
        outward(driven);
        inward(read);
        return true;
    }
    }
    return true;
}

} // namespace

bool bridgeLanguages(eir::Design& design, DiagnosticEngine& diagnostics) {
    // Only a definition instantiating one of the other language is copied and changed: most of a
    // design is of one language, and copying all of it for every elaboration would be the cost.
    const auto crosses = [&design](const eir::ModuleDefinition& definition) {
        return std::ranges::any_of(definition.instances, [&](const eir::Instance& instance) {
            const eir::ModuleDefinition* child = design.findModule(instance.definitionName);
            return child != nullptr && child->language != definition.language;
        });
    };

    bool converted = true;
    for (std::uint32_t index = 0; index < design.size(); ++index) {
        if (!crosses(design.module(index))) continue;
        eir::ModuleDefinition parent = design.module(index);
        bool changed = false;
        for (eir::Instance& instance : parent.instances) {
            const eir::ModuleDefinition* child = design.findModule(instance.definitionName);
            if (child == nullptr || child->language == parent.language) continue;

            for (eir::PortConnection& connection : instance.connections) {
                if (!connection.connected() || connection.portIndex >= child->ports.size())
                    continue;
                const eir::Port& port = child->ports[connection.portIndex];
                const std::string base = std::format("{}${}$boundary", instance.name, port.name);

                // Connected through one already, by an earlier run on this design.
                if (parent.objects[connection.objectIndex].name == base) continue;

                if (!bridge(parent, instance, *child, port, connection, base, diagnostics)) {
                    converted = false;
                    continue;
                }
                changed = true;
            }
        }
        if (changed) design.addModule(std::move(parent));
    }
    return converted;
}

std::optional<std::uint32_t> shownWidth(const eir::ModuleDefinition& definition,
                                        const eir::Object& object) {
    std::string refusal;
    const std::optional<std::vector<Leaf>> leaves = leavesOf(definition, object, refusal);
    if (!leaves) return std::nullopt;
    std::uint32_t width = 0;
    for (const Leaf& leaf : *leaves) width += leaf.shownWidth();
    return width;
}

} // namespace enzan::elab
