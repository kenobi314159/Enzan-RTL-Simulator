// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Serialization.h"

#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstring>
#include <map>
#include <optional>
#include <utility>

namespace enzan::eir {

namespace {

/// Eight bytes at the front, so that a file that is not a library is rejected before its
/// version is believed.
constexpr std::string_view Magic = "ENZANEIR";

/// FNV-1a over the payload, written after it.
///
/// A checksum rather than nothing, because the failure it catches is the one that matters: a
/// truncated write — an interrupted build, a full disk — leaves a file that decodes into
/// plausible garbage, and section 4.4 requires a mismatch to cause clean re-analysis rather
/// than silent misbehaviour. It is not a defence against tampering and is not meant as one.
constexpr std::uint64_t FnvOffsetBasis = 0xcbf2'9ce4'8422'2325ULL;
constexpr std::uint64_t FnvPrime = 0x0000'0100'0000'01b3ULL;

std::uint64_t checksum(std::string_view bytes) {
    std::uint64_t hash = FnvOffsetBasis;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= FnvPrime;
    }
    return hash;
}

/// Appends fixed-width little-endian values to a byte string.
class Writer {
public:
    void u8(std::uint8_t value) { bytes.push_back(static_cast<char>(value)); }

    void boolean(bool value) { u8(value ? 1 : 0); }

    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
    }

    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
    }

    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.append(value);
    }

    [[nodiscard]] const std::string& take() const noexcept { return bytes; }

private:
    std::string bytes;
};

/// Reads back what Writer wrote, refusing to run off the end.
///
/// Every read is bounds-checked and a failure is sticky: once `failed` is set every later read
/// returns a default, so a decoder can be written as straight-line code and checked once at
/// the end rather than after every field.
class Reader {
public:
    explicit Reader(std::string_view source) : bytes(source) {}

    std::uint8_t u8() {
        if (failed || position >= bytes.size()) {
            failed = true;
            return 0;
        }
        return static_cast<std::uint8_t>(bytes[position++]);
    }

    bool boolean() { return u8() != 0; }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(u8()) << shift;
        return value;
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(u8()) << shift;
        return value;
    }

    std::string text() {
        const std::uint32_t length = u32();
        if (failed || bytes.size() - position < length) {
            failed = true;
            return {};
        }
        std::string value(bytes.substr(position, length));
        position += length;
        return value;
    }

    /// A count that is about to size a container, refused when it cannot possibly fit.
    ///
    /// Without this a corrupt length reserves gigabytes before the read that would have
    /// failed ever happens, which turns a rejected file into an out-of-memory kill.
    ///
    /// The bound is one element per remaining byte. Every element of every list here occupies
    /// at least a byte, so the bound always holds for a valid file; a tighter estimate per
    /// element would bound the allocation more closely and would reject valid files whenever
    /// the estimate drifted above the truth, which is a far worse way to be wrong.
    std::uint32_t count() {
        const std::uint32_t value = u32();
        if (failed) return 0;
        if (value > bytes.size() - position) {
            failed = true;
            return 0;
        }
        return value;
    }

    /// Bytes not yet read, or zero once a read has failed.
    [[nodiscard]] std::size_t remaining() const noexcept {
        return failed ? 0 : bytes.size() - position;
    }

    [[nodiscard]] bool ok() const noexcept { return !failed; }

    [[nodiscard]] bool atEnd() const noexcept { return position == bytes.size(); }

    void fail() noexcept { failed = true; }

private:
    std::string_view bytes;
    std::size_t position = 0;
    bool failed = false;
};

// ---------------------------------------------------------------------------
// Buffer names
// ---------------------------------------------------------------------------

/// Collects the buffer names a set of units refers to, so that locations can be stored by name.
class NameTable {
public:
    explicit NameTable(const SourceManager& sourceManager) : sources(&sourceManager) {}

    /// The index of `buffer`'s name, adding it when new. `NoBuffer` for an invalid one.
    std::uint32_t indexOf(BufferId buffer) {
        if (!buffer.valid()) return NoBuffer;

        const std::string name(sources->bufferName(buffer));
        const auto existing = indices.find(name);
        if (existing != indices.end()) return existing->second;

        const auto index = static_cast<std::uint32_t>(names.size());
        names.push_back(name);
        indices.emplace(name, index);
        return index;
    }

    [[nodiscard]] const std::vector<std::string>& entries() const noexcept { return names; }

    static constexpr std::uint32_t NoBuffer = 0xFFFF'FFFFU;

private:
    const SourceManager* sources;
    std::vector<std::string> names;

    /// Ordered rather than hashed so that two encodings of the same design are byte-identical,
    /// which is what makes a content-addressed library entry stable (section 3.8).
    std::map<std::string, std::uint32_t, std::less<>> indices;
};

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void writeLocation(Writer& out, NameTable& names, SourceLocation location) {
    out.u32(names.indexOf(location.buffer()));
    out.u32(location.offset());
}

void writeType(Writer& out, const Type& type) {
    out.u8(static_cast<std::uint8_t>(type.kind()));
    out.u32(type.bitWidth());
    out.boolean(type.isSigned());
    out.boolean(type.isFourState());
    out.u32(type.widthExpression());
}

void writeVector(Writer& out, const BitVector& value) {
    out.u32(value.width());
    const ConstBitRef view = value.ref();
    for (const Word word : view.avalWords()) out.u64(word);
    for (const Word word : view.bvalWords()) out.u64(word);
}

void writeOperand(Writer& out, Operand operand) {
    out.u8(static_cast<std::uint8_t>(operand.kind()));
    out.u32(operand.index());
}

/// Writes one instruction, with `previous` carrying the last location written.
///
/// The location is stored because it is what a source-line breakpoint and a step resolve
/// against (Instruction::location): a unit served from the library with its locations dropped
/// decodes cleanly, simulates identically, and silently cannot be debugged.
///
/// A repeat costs one byte rather than eight. Nearly every instruction shares the location of
/// the statement that produced it — that is the premise the codegen line table is built on
/// (codegen/Bytecode.h) — so a flag per instruction is smaller than a location per instruction
/// by about the ratio between the two.
void writeInstruction(Writer& out, NameTable& names, const Instruction& instruction,
                      SourceLocation& previous) {
    out.u32(static_cast<std::uint32_t>(instruction.opcode));
    out.boolean(instruction.isSigned);
    writeOperand(out, instruction.dst);
    writeOperand(out, instruction.a);
    writeOperand(out, instruction.b);
    writeOperand(out, instruction.c);

    if (instruction.location == previous) {
        out.boolean(false);
        return;
    }

    out.boolean(true);
    writeLocation(out, names, instruction.location);
    previous = instruction.location;
}

void writeIndices(Writer& out, const std::vector<std::uint32_t>& indices) {
    out.u32(static_cast<std::uint32_t>(indices.size()));
    for (const std::uint32_t index : indices) out.u32(index);
}

void writeProcess(Writer& out, NameTable& names, const Process& process) {
    out.u8(static_cast<std::uint8_t>(process.kind));
    out.text(process.name);
    writeLocation(out, names, process.location);

    out.u32(static_cast<std::uint32_t>(process.registerTypes.size()));
    for (const Type& type : process.registerTypes) writeType(out, type);

    out.u32(static_cast<std::uint32_t>(process.constants.size()));
    for (const BitVector& value : process.constants) writeVector(out, value);

    writeIndices(out, process.constantExpressions);

    out.u32(static_cast<std::uint32_t>(process.strings.size()));
    for (const std::string& value : process.strings) out.text(value);

    // One running location for the whole process rather than one per block, so that a block
    // whose first statement continues the previous block's still pays a byte for it. The
    // decoder threads the same value through in the same order, which is what makes the two
    // agree without the encoding having to say where the run began.
    SourceLocation previousLocation;

    out.u32(static_cast<std::uint32_t>(process.blocks.size()));
    for (const BasicBlock& block : process.blocks) {
        out.text(block.label);
        out.u32(static_cast<std::uint32_t>(block.instructions.size()));
        for (const Instruction& instruction : block.instructions)
            writeInstruction(out, names, instruction, previousLocation);
    }

    out.u32(static_cast<std::uint32_t>(process.sysCalls.size()));
    for (const SysTaskCall& call : process.sysCalls) {
        out.u8(static_cast<std::uint8_t>(call.kind));
        out.u32(call.formatIndex);
        out.u32(call.objectIndex);
        writeLocation(out, names, call.location);
        out.u32(static_cast<std::uint32_t>(call.arguments.size()));
        for (const SysTaskArgument& argument : call.arguments) {
            writeOperand(out, argument.value);
            writeType(out, argument.type);
        }
    }

    out.u32(static_cast<std::uint32_t>(process.sensitivity.size()));
    const auto writeEntry = [&out](const SensitivityEntry& entry) {
        out.u32(entry.objectIndex);
        out.u8(static_cast<std::uint8_t>(entry.edge));
        out.u32(entry.lowBit);
        out.u32(entry.width);
        out.u32(entry.lowBitExpression);
        out.u32(entry.widthExpression);
    };
    for (const SensitivityEntry& entry : process.sensitivity) writeEntry(entry);
    out.u32(static_cast<std::uint32_t>(process.eventLists.size()));
    for (const std::vector<SensitivityEntry>& list : process.eventLists) {
        out.u32(static_cast<std::uint32_t>(list.size()));
        for (const SensitivityEntry& entry : list) writeEntry(entry);
    }

    out.u32(static_cast<std::uint32_t>(process.readSet.size()));
    for (const std::uint32_t index : process.readSet) out.u32(index);

    out.u32(static_cast<std::uint32_t>(process.writeSet.size()));
    for (const WriteExtent& extent : process.writeSet) {
        out.u32(extent.objectIndex);
        out.u32(extent.lowBit);
        out.u32(extent.width);
        out.u32(extent.lowBitExpression);
        out.u32(extent.widthExpression);
    }
}

void writeModule(Writer& out, NameTable& names, const ModuleDefinition& definition) {
    out.text(definition.name);
    out.text(definition.sourceName);
    out.boolean(definition.isPackage);
    out.boolean(definition.isFunction);
    writeLocation(out, names, definition.location);

    out.u32(static_cast<std::uint32_t>(definition.objects.size()));
    for (const Object& object : definition.objects) {
        out.text(object.name);
        writeType(out, object.type);
        out.u8(static_cast<std::uint8_t>(object.kind));
        out.u8(static_cast<std::uint8_t>(object.netKind));
        writeLocation(out, names, object.location);
        out.boolean(object.initialValue.has_value());
        if (object.initialValue.has_value()) writeVector(out, *object.initialValue);
        out.boolean(object.isAlias);
        out.u32(object.enumIndex);
        out.u32(static_cast<std::uint32_t>(object.unpackedDimensions.size()));
        for (const UnpackedDimension& dimension : object.unpackedDimensions) {
            out.u32(static_cast<std::uint32_t>(dimension.left));
            out.u32(static_cast<std::uint32_t>(dimension.right));
            out.u32(dimension.leftExpression);
            out.u32(dimension.rightExpression);
        }
        out.u32(object.elementWidth);
        out.boolean(object.elementSigned);
        out.boolean(object.subroutineStatic);
        out.text(object.hierarchicalPath);
        out.u32(object.elementWidthExpression);
        out.u32(object.initialValueExpression);
    }

    out.u32(static_cast<std::uint32_t>(definition.ports.size()));
    for (const Port& port : definition.ports) {
        out.text(port.name);
        out.u8(static_cast<std::uint8_t>(port.direction));
        out.u32(port.objectIndex);
        writeLocation(out, names, port.location);
    }

    out.u32(static_cast<std::uint32_t>(definition.instances.size()));
    for (const Instance& instance : definition.instances) {
        out.text(instance.name);
        out.text(instance.definitionName);
        writeLocation(out, names, instance.location);
        out.u32(static_cast<std::uint32_t>(instance.connections.size()));
        for (const PortConnection& connection : instance.connections) {
            out.u32(connection.portIndex);
            out.u32(connection.objectIndex);
        }
        out.u32(static_cast<std::uint32_t>(instance.interfaceBindings.size()));
        for (const InterfaceBinding& binding : instance.interfaceBindings) {
            out.text(binding.port);
            out.text(binding.target);
        }
        out.u32(static_cast<std::uint32_t>(instance.parameterActuals.size()));
        for (const ParameterActual& actual : instance.parameterActuals) {
            out.text(actual.formal);
            out.u32(actual.value);
        }
    }

    out.u32(static_cast<std::uint32_t>(definition.processes.size()));
    for (const Process& process : definition.processes) writeProcess(out, names, process);

    out.u32(static_cast<std::uint32_t>(definition.enums.size()));
    for (const EnumDefinition& enumeration : definition.enums) {
        out.text(enumeration.name);
        out.u32(static_cast<std::uint32_t>(enumeration.members.size()));
        for (const EnumMember& member : enumeration.members) {
            out.text(member.name);
            writeVector(out, member.value);
        }
    }

    out.u32(static_cast<std::uint32_t>(definition.parameters.size()));
    for (const Parameter& parameter : definition.parameters) {
        out.text(parameter.name);
        writeVector(out, parameter.value);
        out.boolean(parameter.isLocal);
        writeLocation(out, names, parameter.location);
    }

    out.u32(static_cast<std::uint32_t>(definition.formals.size()));
    for (const ParameterFormal& formal : definition.formals) {
        out.text(formal.name);
        out.u8(static_cast<std::uint8_t>(formal.kind));
        out.text(formal.port);
        out.u32(formal.width);
        out.u32(formal.widthExpression);
        out.boolean(formal.isSigned);
        out.boolean(formal.isTyped);
        out.u32(formal.value);
        writeLocation(out, names, formal.location);
    }

    const ParameterExpressions& expressions = definition.expressions;
    out.u32(static_cast<std::uint32_t>(expressions.allConstants().size()));
    for (const BitVector& value : expressions.allConstants()) writeVector(out, value);
    out.u32(static_cast<std::uint32_t>(expressions.allFunctions().size()));
    for (const std::string& function : expressions.allFunctions()) out.text(function);
    out.u32(static_cast<std::uint32_t>(expressions.size()));
    for (const ParameterNode& node : expressions.allNodes()) {
        out.u8(static_cast<std::uint8_t>(node.kind));
        out.u8(static_cast<std::uint8_t>(node.opcode));
        out.boolean(node.isSigned);
        out.boolean(node.operationSigned);
        out.u32(node.width);
        out.u32(node.widthNode);
        out.u32(node.a);
        out.u32(node.b);
        out.u32(node.c);
    }
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Resolves a stored buffer index back to the caller's source manager.
class BufferResolver {
public:
    BufferResolver(std::vector<std::string> storedNames,
                   const std::function<BufferId(std::string_view)>& resolve)
        : names(std::move(storedNames)), resolver(&resolve), resolved(names.size()) {}

    BufferId operator()(std::uint32_t index) {
        if (index >= names.size()) return {};
        if (!attempted[index]) {
            attempted[index] = true;
            resolved[index] = (*resolver)(names[index]);
        }
        return resolved[index];
    }

private:
    std::vector<std::string> names;
    const std::function<BufferId(std::string_view)>* resolver;

    /// Cached, because a file is resolved once however many locations point into it and
    /// resolving usually means reading it.
    std::vector<BufferId> resolved;
    std::vector<bool> attempted{std::vector<bool>(names.size(), false)};
};

SourceLocation readLocation(Reader& in, BufferResolver& buffers) {
    const std::uint32_t index = in.u32();
    const std::uint32_t offset = in.u32();
    const BufferId buffer = buffers(index);
    if (!buffer.valid()) return {};
    return {buffer, offset};
}

Type readType(Reader& in) {
    const auto kind = static_cast<Type::Kind>(in.u8());
    const std::uint32_t width = in.u32();
    const bool isSigned = in.boolean();
    const bool isFourState = in.boolean();
    const std::uint32_t widthExpression = in.u32();

    switch (kind) {
    case Type::Kind::Void: return Type::voidType();
    case Type::Kind::Bits:
        return Type::bits(width, isSigned, isFourState).withWidthExpression(widthExpression);
    case Type::Kind::Real: return Type::real();
    case Type::Kind::String: return Type::string();
    }

    in.fail();
    return Type::voidType();
}

BitVector readVector(Reader& in) {
    // A *bit* width, so `count` — which bounds a list at one byte per element — is the wrong
    // check: sixty-four bits occupy eight bytes, not sixty-four. What the width implies is
    // worked out through `storageWordsFor`, which counts both planes, and checked against
    // what is left.
    const std::uint32_t width = in.u32();
    const std::size_t needed = static_cast<std::size_t>(storageWordsFor(width)) * sizeof(Word);

    // A vector has at least one bit — `BitVector` says so in an assertion — so a stored width
    // of zero is a corrupt file rather than a representable value. One bit of X is returned so
    // that the failure is reported by the caller's `ok()` check rather than by an abort inside
    // a constructor, which is the difference between "this library is stale" and a crash.
    if (!in.ok() || width == 0 || in.remaining() < needed) {
        in.fail();
        return BitVector::filled(1, Logic::Unknown);
    }

    BitVector value(width);
    const BitRef view = value.mutableRef();
    for (Word& word : view.avalWords()) word = in.u64();
    for (Word& word : view.bvalWords()) word = in.u64();
    return value;
}

Operand readOperand(Reader& in) {
    const auto kind = static_cast<Operand::Kind>(in.u8());
    const std::uint32_t index = in.u32();

    switch (kind) {
    case Operand::Kind::None: return Operand::none();
    case Operand::Kind::Register: return Operand::reg(index);
    case Operand::Kind::Constant: return Operand::constant(index);
    case Operand::Kind::Object: return Operand::object(index);
    case Operand::Kind::Block: return Operand::block(index);
    case Operand::Kind::SysCall: return Operand::sysCall(index);
    case Operand::Kind::Immediate: return Operand::immediate(index);
    }

    in.fail();
    return Operand::none();
}

Instruction readInstruction(Reader& in, BufferResolver& buffers, SourceLocation& previous) {
    Instruction instruction;
    instruction.opcode = static_cast<Opcode>(in.u32());
    instruction.isSigned = in.boolean();
    instruction.dst = readOperand(in);
    instruction.a = readOperand(in);
    instruction.b = readOperand(in);
    instruction.c = readOperand(in);

    // A location the writer marked as a repeat is not stored, so it is taken from the run it
    // continues. The writer threads `previous` through in this same order.
    if (in.boolean()) previous = readLocation(in, buffers);
    instruction.location = previous;
    return instruction;
}

Process readProcess(Reader& in, BufferResolver& buffers) {
    Process process;
    process.kind = static_cast<ProcessKind>(in.u8());
    process.name = in.text();
    process.location = readLocation(in, buffers);

    process.registerTypes.resize(in.count());
    for (Type& type : process.registerTypes) type = readType(in);

    const std::uint32_t constantCount = in.count();
    process.constants.reserve(constantCount);
    for (std::uint32_t index = 0; index < constantCount && in.ok(); ++index)
        process.constants.push_back(readVector(in));

    process.constantExpressions.resize(in.count());
    for (std::uint32_t& index : process.constantExpressions) index = in.u32();

    process.strings.resize(in.count());
    for (std::string& value : process.strings) value = in.text();

    // The running location the writer threaded through the same traversal.
    SourceLocation previousLocation;

    process.blocks.resize(in.count());
    for (BasicBlock& block : process.blocks) {
        block.label = in.text();
        block.instructions.resize(in.count());
        for (Instruction& instruction : block.instructions)
            instruction = readInstruction(in, buffers, previousLocation);
    }

    process.sysCalls.resize(in.count());
    for (SysTaskCall& call : process.sysCalls) {
        call.kind = static_cast<SysTaskKind>(in.u8());
        call.formatIndex = in.u32();
        call.objectIndex = in.u32();
        call.location = readLocation(in, buffers);
        call.arguments.resize(in.count());
        for (SysTaskArgument& argument : call.arguments) {
            argument.value = readOperand(in);
            argument.type = readType(in);
        }
    }

    const auto readEntry = [&in](SensitivityEntry& entry) {
        entry.objectIndex = in.u32();
        entry.edge = static_cast<EdgeKind>(in.u8());
        entry.lowBit = in.u32();
        entry.width = in.u32();
        entry.lowBitExpression = in.u32();
        entry.widthExpression = in.u32();
    };
    process.sensitivity.resize(in.count());
    for (SensitivityEntry& entry : process.sensitivity) readEntry(entry);
    process.eventLists.resize(in.count());
    for (std::vector<SensitivityEntry>& list : process.eventLists) {
        list.resize(in.count());
        for (SensitivityEntry& entry : list) readEntry(entry);
    }

    process.readSet.resize(in.count());
    for (std::uint32_t& index : process.readSet) index = in.u32();

    process.writeSet.resize(in.count());
    for (WriteExtent& extent : process.writeSet) {
        extent.objectIndex = in.u32();
        extent.lowBit = in.u32();
        extent.width = in.u32();
        extent.lowBitExpression = in.u32();
        extent.widthExpression = in.u32();
    }

    return process;
}

ModuleDefinition readModule(Reader& in, BufferResolver& buffers) {
    ModuleDefinition definition;
    definition.name = in.text();
    definition.sourceName = in.text();
    definition.isPackage = in.boolean();
    definition.isFunction = in.boolean();
    definition.location = readLocation(in, buffers);

    definition.objects.resize(in.count());
    for (Object& object : definition.objects) {
        object.name = in.text();
        object.type = readType(in);
        object.kind = static_cast<ObjectKind>(in.u8());
        object.netKind = static_cast<NetKind>(in.u8());
        object.location = readLocation(in, buffers);
        if (in.boolean()) object.initialValue = readVector(in);
        object.isAlias = in.boolean();
        object.enumIndex = in.u32();
        object.unpackedDimensions.resize(in.count());
        for (UnpackedDimension& dimension : object.unpackedDimensions) {
            dimension.left = static_cast<std::int32_t>(in.u32());
            dimension.right = static_cast<std::int32_t>(in.u32());
            dimension.leftExpression = in.u32();
            dimension.rightExpression = in.u32();
        }
        object.elementWidth = in.u32();
        object.elementSigned = in.boolean();
        object.subroutineStatic = in.boolean();
        object.hierarchicalPath = in.text();
        object.elementWidthExpression = in.u32();
        object.initialValueExpression = in.u32();
    }

    definition.ports.resize(in.count());
    for (Port& port : definition.ports) {
        port.name = in.text();
        port.direction = static_cast<PortDirection>(in.u8());
        port.objectIndex = in.u32();
        port.location = readLocation(in, buffers);
    }

    definition.instances.resize(in.count());
    for (Instance& instance : definition.instances) {
        instance.name = in.text();
        instance.definitionName = in.text();
        instance.location = readLocation(in, buffers);
        instance.connections.resize(in.count());
        for (PortConnection& connection : instance.connections) {
            connection.portIndex = in.u32();
            connection.objectIndex = in.u32();
        }
        instance.interfaceBindings.resize(in.count());
        for (InterfaceBinding& binding : instance.interfaceBindings) {
            binding.port = in.text();
            binding.target = in.text();
        }
        instance.parameterActuals.resize(in.count());
        for (ParameterActual& actual : instance.parameterActuals) {
            actual.formal = in.text();
            actual.value = in.u32();
        }
    }

    const std::uint32_t processCount = in.count();
    definition.processes.reserve(processCount);
    for (std::uint32_t index = 0; index < processCount && in.ok(); ++index)
        definition.processes.push_back(readProcess(in, buffers));

    definition.enums.resize(in.count());
    for (EnumDefinition& enumeration : definition.enums) {
        enumeration.name = in.text();
        const std::uint32_t memberCount = in.count();
        enumeration.members.reserve(memberCount);
        for (std::uint32_t index = 0; index < memberCount && in.ok(); ++index) {
            std::string name = in.text();
            BitVector value = readVector(in);
            enumeration.members.push_back(
                EnumMember{.name = std::move(name), .value = std::move(value)});
        }
    }

    const std::uint32_t parameterCount = in.count();
    definition.parameters.reserve(parameterCount);
    for (std::uint32_t index = 0; index < parameterCount && in.ok(); ++index) {
        // Read into locals, one statement per field. A braced initialiser would be correct —
        // its elements are evaluated left to right — but every field here comes out of a
        // stream in a fixed order, and writing that order as statements says so to the next
        // reader instead of relying on them knowing the rule.
        std::string name = in.text();
        BitVector value = readVector(in);
        const bool isLocal = in.boolean();
        const SourceLocation location = readLocation(in, buffers);
        definition.parameters.push_back(Parameter{.name = std::move(name),
                                                  .value = std::move(value),
                                                  .isLocal = isLocal,
                                                  .location = location});
    }

    definition.formals.resize(in.count());
    for (ParameterFormal& formal : definition.formals) {
        formal.name = in.text();
        formal.kind = static_cast<FormalKind>(in.u8());
        formal.port = in.text();
        formal.width = in.u32();
        formal.widthExpression = in.u32();
        formal.isSigned = in.boolean();
        formal.isTyped = in.boolean();
        formal.value = in.u32();
        formal.location = readLocation(in, buffers);
    }

    const std::uint32_t expressionConstantCount = in.count();
    std::vector<BitVector> expressionConstants;
    expressionConstants.reserve(expressionConstantCount);
    for (std::uint32_t index = 0; index < expressionConstantCount && in.ok(); ++index)
        expressionConstants.push_back(readVector(in));
    std::vector<std::string> functions(in.count());
    for (std::string& function : functions) function = in.text();
    std::vector<ParameterNode> nodes(in.count());
    for (ParameterNode& node : nodes) {
        const std::uint8_t kind = in.u8();
        if (kind > static_cast<std::uint8_t>(ParameterNodeKind::Call)) in.fail();
        node.kind = static_cast<ParameterNodeKind>(kind);
        node.opcode = static_cast<Opcode>(in.u8());
        node.isSigned = in.boolean();
        node.operationSigned = in.boolean();
        node.width = in.u32();
        node.widthNode = in.u32();
        node.a = in.u32();
        node.b = in.u32();
        node.c = in.u32();
    }
    // A table no `add` could have built — a node using one after it, a constant that does not
    // exist — would make evaluation read past the table or loop, so it fails the decode as a
    // truncated entry does.
    std::optional<ParameterExpressions> expressions = ParameterExpressions::fromParts(
        std::move(nodes), std::move(expressionConstants), std::move(functions));
    if (!expressions.has_value()) {
        in.fail();
        return definition;
    }
    definition.expressions = std::move(*expressions);

    return definition;
}

} // namespace

std::string encodeModules(std::span<const ModuleDefinition> modules, const SourceManager& sources) {
    // The units are encoded first, because encoding is what discovers which buffer names the
    // table needs. The header and the table are then prepended.
    NameTable names(sources);
    Writer body;
    body.u32(static_cast<std::uint32_t>(modules.size()));
    for (const ModuleDefinition& definition : modules) writeModule(body, names, definition);

    Writer table;
    table.u32(static_cast<std::uint32_t>(names.entries().size()));
    for (const std::string& name : names.entries()) table.text(name);

    std::string payload = table.take();
    payload += body.take();

    Writer out;
    for (const char byte : Magic) out.u8(static_cast<std::uint8_t>(byte));
    out.u32(SerializationFormatVersion);
    out.u64(payload.size());
    out.u64(checksum(payload));

    std::string result = out.take();
    result += payload;
    return result;
}

bool decodeModules(std::string_view bytes,
                   const std::function<BufferId(std::string_view)>& resolveBuffer,
                   std::vector<ModuleDefinition>& modules) {
    constexpr std::size_t HeaderSize = 8 + 4 + 8 + 8;
    if (bytes.size() < HeaderSize) return false;
    if (!bytes.starts_with(Magic)) return false;

    Reader header(bytes.substr(Magic.size(), HeaderSize - Magic.size()));
    if (header.u32() != SerializationFormatVersion) return false;

    const std::uint64_t payloadSize = header.u64();
    const std::uint64_t expected = header.u64();
    if (!header.ok()) return false;

    const std::string_view payload = bytes.substr(HeaderSize);
    if (payload.size() != payloadSize) return false;
    if (checksum(payload) != expected) return false;

    Reader in(payload);

    std::vector<std::string> names;
    names.resize(in.count());
    for (std::string& name : names) name = in.text();
    if (!in.ok()) return false;

    BufferResolver buffers(std::move(names), resolveBuffer);

    // Decoded into a local list and moved out only on success, so that a file that fails half
    // way leaves the caller's design exactly as it was.
    std::vector<ModuleDefinition> decoded;
    const std::uint32_t moduleCount = in.count();
    decoded.reserve(moduleCount);
    for (std::uint32_t index = 0; index < moduleCount && in.ok(); ++index)
        decoded.push_back(readModule(in, buffers));

    if (!in.ok() || !in.atEnd()) return false;

    for (ModuleDefinition& definition : decoded) modules.push_back(std::move(definition));
    return true;
}

} // namespace enzan::eir
