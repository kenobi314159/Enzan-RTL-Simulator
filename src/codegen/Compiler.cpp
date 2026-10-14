// SPDX-License-Identifier: GPL-3.0-or-later

#include "codegen/Compiler.h"

#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "eir/BitOps.h"
#include "eir/Module.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace enzan::codegen {

std::uint32_t CompiledDesign::findBody(const std::vector<Key>& keys, std::uint32_t definitionIndex,
                                       std::uint32_t index) {
    const auto found = std::ranges::find_if(keys, [&](const Key& key) {
        return key.definitionIndex == definitionIndex && key.processIndex == index;
    });
    return found == keys.end() ? NotCompiled : found->bodyIndex;
}

std::uint32_t CompiledDesign::bodyIndex(std::uint32_t definitionIndex,
                                        std::uint32_t processIndex) const {
    return findBody(keys, definitionIndex, processIndex);
}

std::uint32_t CompiledDesign::subprogramBodyIndex(std::uint32_t definitionIndex,
                                                  std::uint32_t subprogramIndex) const {
    return findBody(subprogramKeys, definitionIndex, subprogramIndex);
}

namespace {

/// True for the opcodes that write an object, which are the ones the runtime routes.
bool isStore(eir::Opcode opcode) noexcept {
    return opcode == eir::Opcode::Store || opcode == eir::Opcode::StorePart
           || opcode == eir::Opcode::StoreNonBlocking || opcode == eir::Opcode::StorePartNonBlocking
           || opcode == eir::Opcode::StoreDelayed || opcode == eir::Opcode::StoreNonBlockingDelayed
           || opcode == eir::Opcode::Drive || opcode == eir::Opcode::DriveTransport
           || opcode == eir::Opcode::DrivePart || opcode == eir::Opcode::DrivePartAfter
           || opcode == eir::Opcode::DriveTransportPart;
}

/// Assigns each virtual register a Word offset in the frame and records its width.
///
/// Every register lives in the frame, wide ones included. That is what section 6.1
/// means by spilling wide vectors to a per-process scratch area: there is only one
/// place a value can be, so nothing has to decide when to spill.
struct Frame {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> widths;
    std::uint32_t words = 0;

    void build(const std::vector<eir::Type>& registerTypes) {
        offsets.reserve(registerTypes.size());
        widths.reserve(registerTypes.size());

        for (const eir::Type& type : registerTypes) {
            offsets.push_back(words);
            widths.push_back(type.bitWidth());
            words += type.storageWords();
        }
    }
};

/// For each register of `body`, the constant load that is the only thing to write it, or null.
/// Such a register holds that constant from the moment the load runs, and nothing could have
/// read it before, so it can hold it from the start. `parameters` are the registers a caller
/// writes, and the arguments and results of calls are written by the call, so neither is one.
std::vector<const eir::Instruction*> constantLoads(const eir::Body& body,
                                                   const std::vector<std::uint32_t>& parameters) {
    const std::size_t count = body.registerTypes.size();
    std::vector<const eir::Instruction*> loads(count, nullptr);
    std::vector<bool> written(count, false);
    const auto writeOther = [&](eir::Operand operand) {
        if (operand.kind() != eir::Operand::Kind::Register || operand.index() >= count) return;
        written[operand.index()] = true;
        loads[operand.index()] = nullptr;
    };
    for (const eir::BasicBlock& block : body.blocks) {
        for (const eir::Instruction& instruction : block.instructions) {
            const eir::Operand dst = instruction.dst;
            if (dst.kind() != eir::Operand::Kind::Register || dst.index() >= count) continue;
            if (instruction.opcode != eir::Opcode::LoadConstant || written[dst.index()]) {
                writeOther(dst);
                continue;
            }
            written[dst.index()] = true;
            loads[dst.index()] = &instruction;
        }
    }
    for (const eir::CallSite& call : body.calls) {
        for (const eir::Operand argument : call.arguments) writeOther(argument);
        writeOther(call.result);
    }
    for (const std::uint32_t parameter : parameters) writeOther(eir::Operand::reg(parameter));
    return loads;
}

/// A branch target that cannot be resolved until every block has been emitted.
struct Fixup {
    std::uint32_t instructionIndex = 0;

    /// Which operand field holds the block index: 0 for `a`, 1 for `b`, 2 for `c`.
    std::uint8_t field = 0;

    std::uint32_t blockIndex = 0;
};

/// Returns a pointer to the numbered operand field of `instruction`.
std::uint32_t* operandField(Instruction& instruction, std::uint8_t field) noexcept {
    switch (field) {
    case 0: return &instruction.a;
    case 1: return &instruction.b;
    default: return &instruction.c;
    }
}

} // namespace

Compiler::Compiler(DiagnosticEngine& engine) noexcept : diagnostics(engine) {}

std::unique_ptr<CompiledDesign> Compiler::compile(const eir::Design& design) {
    auto result = std::make_unique<CompiledDesign>();

    for (std::uint32_t definitionIndex = 0; definitionIndex < design.size(); ++definitionIndex) {
        const eir::ModuleDefinition& definition = design.module(definitionIndex);

        for (std::uint32_t processIndex = 0; processIndex < definition.processes.size();
             ++processIndex) {
            CompiledBody body;
            if (!compileProcess(definition, definition.processes[processIndex], body))
                return nullptr;

            const auto compiledIndex = static_cast<std::uint32_t>(result->processes.size());
            result->processes.push_back(std::move(body));
            result->keys.push_back(CompiledDesign::Key{.definitionIndex = definitionIndex,
                                                       .processIndex = processIndex,
                                                       .bodyIndex = compiledIndex});
        }

        for (std::uint32_t subprogramIndex = 0; subprogramIndex < definition.subprograms.size();
             ++subprogramIndex) {
            CompiledBody body;
            if (!compileSubprogram(definition, definition.subprograms[subprogramIndex], body))
                return nullptr;

            const auto compiledIndex = static_cast<std::uint32_t>(result->processes.size());
            result->processes.push_back(std::move(body));
            result->subprogramKeys.push_back(CompiledDesign::Key{.definitionIndex = definitionIndex,
                                                                 .processIndex = subprogramIndex,
                                                                 .bodyIndex = compiledIndex});
        }
    }

    return result;
}

bool Compiler::compileProcess(const eir::ModuleDefinition& definition, const eir::Process& process,
                              CompiledBody& out) {
    out.kind = process.kind;
    out.resumesNextCycle = process.drivesSignals;
    out.postponed = process.postponed;
    out.connectsPort = process.connectsPort;
    out.sensitivity = process.sensitivity;
    out.eventLists = process.eventLists;
    out.startsAtTimeZero = eir::startsAtTimeZero(process);
    return compileBody(definition, process, out);
}

bool Compiler::compileSubprogram(const eir::ModuleDefinition& definition,
                                 const eir::Subprogram& subprogram, CompiledBody& out) {
    std::vector<std::uint32_t> parameters;
    parameters.reserve(subprogram.parameters.size());
    for (const eir::SubprogramParameter& parameter : subprogram.parameters)
        parameters.push_back(parameter.registerIndex);
    if (!compileBody(definition, subprogram, out, parameters)) return false;

    Frame frame;
    frame.build(subprogram.registerTypes);
    for (const eir::SubprogramParameter& parameter : subprogram.parameters) {
        if (parameter.registerIndex >= frame.offsets.size()) {
            reportInvalid(definition, subprogram,
                          std::format("parameter '{}' is register {}, which is out of range; run "
                                      "the EIR verifier before compiling",
                                      parameter.name, parameter.registerIndex));
            return false;
        }
        out.parameters.push_back(
            CompiledBody::Parameter{.offset = frame.offsets[parameter.registerIndex],
                                    .bitWidth = frame.widths[parameter.registerIndex],
                                    .mode = parameter.mode});
    }
    if (subprogram.isFunction()) {
        if (subprogram.resultRegister >= frame.offsets.size()) {
            reportInvalid(definition, subprogram,
                          "its result register is out of range; run the EIR verifier before "
                          "compiling");
            return false;
        }
        out.resultOffset = frame.offsets[subprogram.resultRegister];
        out.resultWidth = frame.widths[subprogram.resultRegister];
    }
    return true;
}

bool Compiler::compileBody(const eir::ModuleDefinition& definition, const eir::Body& body,
                           CompiledBody& out, const std::vector<std::uint32_t>& parameters) {
    out.name = body.name;
    out.location = body.location;
    out.readSet = body.readSet;
    out.writeSet = body.writeSet;
    out.strings = body.strings;

    Frame frame;
    frame.build(body.registerTypes);
    out.frameWords = frame.words;

    // Constants are laid out end to end in the same two-plane form the arena uses, so
    // that reading one is a view over the pool rather than a copy.
    out.constants.reserve(body.constants.size());
    for (const eir::BitVector& value : body.constants) {
        const auto offset = static_cast<std::uint32_t>(out.constantPool.size());
        out.constants.push_back(Constant{.wordOffset = offset, .bitWidth = value.width()});

        const eir::ConstBitRef source = value.ref();
        out.constantPool.insert(out.constantPool.end(), source.avalWords().begin(),
                                source.avalWords().end());
        out.constantPool.insert(out.constantPool.end(), source.bvalWords().begin(),
                                source.bvalWords().end());
    }

    if (!compileSysTaskCalls(definition, body, frame.offsets, frame.widths, out)) return false;
    if (!compileCalls(definition, body, frame.offsets, frame.widths, out)) return false;

    // A register that holds one constant throughout holds it in the frame's starting image, and
    // its load is left out. Most of what a body loads is such a constant, an index's bound or an
    // element's width, and a load left in runs every time it is reached.
    const std::vector<const eir::Instruction*> loads = constantLoads(body, parameters);
    out.frameImage.assign(frame.words, 0);
    for (std::uint32_t index = 0; index < loads.size(); ++index) {
        const eir::Instruction* load = loads[index];
        if (load == nullptr) continue;
        if (load->a.index() >= body.constants.size()) {
            reportInvalid(definition, body,
                          std::format("constant {} is out of range; run the EIR verifier before "
                                      "compiling",
                                      load->a.index()));
            return false;
        }
        eir::bitops::extend(
            eir::BitRef(out.frameImage.data() + frame.offsets[index], frame.widths[index]),
            body.constants[load->a.index()].ref(), load->isSigned);
    }

    std::vector<Fixup> fixups;
    out.blockStarts.reserve(body.blocks.size());

    const std::vector<std::uint32_t> written = eir::writtenObjects(body.writeSet);

    for (const eir::BasicBlock& block : body.blocks) {
        out.blockStarts.push_back(static_cast<std::uint32_t>(out.code.size()));

        for (const eir::Instruction& source : block.instructions) {
            if (source.opcode == eir::Opcode::LoadConstant
                && source.dst.kind() == eir::Operand::Kind::Register
                && source.dst.index() < loads.size() && loads[source.dst.index()] == &source)
                continue;

            Instruction emitted;
            emitted.opcode = source.opcode;
            emitted.isSigned = source.isSigned;
            emitted.isReal = source.a.kind() == eir::Operand::Kind::Register
                             && source.a.index() < body.registerTypes.size()
                             && body.registerTypes[source.a.index()].isReal();

            // An operand is resolved by its own kind. EIR-B fixes an operand's kind per
            // opcode, so nothing here has to know which opcode it is looking at — which
            // is what keeps this loop from becoming a second switch over the whole
            // instruction set that could drift out of step with the first.
            const std::array<eir::Operand, 3> sources{source.a, source.b, source.c};
            std::array<std::uint32_t*, 3> targets{&emitted.a, &emitted.b, &emitted.c};
            std::array<std::uint32_t*, 3> widths{&emitted.aWidth, &emitted.bWidth, &emitted.cWidth};

            const auto resolve = [&](eir::Operand operand, std::uint32_t& value,
                                     std::uint32_t& width) -> bool {
                value = operand.index();
                width = 0;

                if (operand.kind() != eir::Operand::Kind::Register) return true;

                if (operand.index() >= frame.offsets.size()) {
                    reportInvalid(definition, body,
                                  std::format("register {} is out of range; run the EIR "
                                              "verifier before compiling",
                                              operand.index()));
                    return false;
                }
                value = frame.offsets[operand.index()];
                width = frame.widths[operand.index()];
                return true;
            };

            if (!resolve(source.dst, emitted.dst, emitted.dstWidth)) return false;
            for (std::size_t index = 0; index < sources.size(); ++index) {
                if (!resolve(sources[index], *targets[index], *widths[index])) return false;
            }

            // A store has no destination register, so `dst` carries its object's position
            // among the objects the process writes, which is what the runtime routes it by:
            // straight into the signal, or through the process's drivers of it (section 3.7).
            if (isStore(source.opcode)) {
                const auto position = std::ranges::lower_bound(written, source.a.index());
                if (position == written.end() || *position != source.a.index()) {
                    reportInvalid(definition, body,
                                  std::format("a store to object {} is not in the write set; run "
                                              "the EIR verifier before compiling",
                                              source.a.index()));
                    return false;
                }
                emitted.dst = static_cast<std::uint32_t>(position - written.begin());
            }

            const auto instructionIndex = static_cast<std::uint32_t>(out.code.size());
            for (std::size_t index = 0; index < sources.size(); ++index) {
                if (sources[index].kind() != eir::Operand::Kind::Block) continue;
                if (sources[index].index() >= body.blocks.size()) {
                    reportInvalid(definition, body,
                                  std::format("branch to block {}, but the body has {}",
                                              sources[index].index(), body.blocks.size()));
                    return false;
                }
                fixups.push_back(Fixup{.instructionIndex = instructionIndex,
                                       .field = static_cast<std::uint8_t>(index),
                                       .blockIndex = sources[index].index()});
            }

            out.code.push_back(emitted);
        }
    }

    // A block's start is only known once every earlier block has been emitted, so
    // branch targets are patched here rather than guessed during emission.
    for (const Fixup& fixup : fixups) {
        ENZAN_ASSERT(fixup.instructionIndex < out.code.size(), "fixup points past the code");
        *operandField(out.code[fixup.instructionIndex], fixup.field) =
            out.blockStarts[fixup.blockIndex];
    }

    return true;
}

bool Compiler::compileSysTaskCalls(const eir::ModuleDefinition& definition, const eir::Body& body,
                                   const std::vector<std::uint32_t>& registerOffsets,
                                   const std::vector<std::uint32_t>& registerWidths,
                                   CompiledBody& out) {
    out.sysCalls.reserve(body.sysCalls.size());

    for (const eir::SysTaskCall& call : body.sysCalls) {
        SysTaskDescriptor descriptor;
        descriptor.kind = call.kind;
        descriptor.formatIndex = call.formatIndex;
        descriptor.objectIndex = call.objectIndex;
        descriptor.location = call.location;

        if (call.hasFormat() && call.formatIndex >= body.strings.size()) {
            reportInvalid(definition, body,
                          std::format("{} names string {}, but the body has {}",
                                      eir::toString(call.kind), call.formatIndex,
                                      body.strings.size()));
            return false;
        }

        for (const eir::SysTaskArgument& argument : call.arguments) {
            SysTaskDescriptor::Argument compiled;
            compiled.isSigned = argument.type.isSigned();
            compiled.bitWidth = argument.type.bitWidth();

            switch (argument.value.kind()) {
            case eir::Operand::Kind::Register:
                if (argument.value.index() >= registerOffsets.size()) {
                    reportInvalid(definition, body,
                                  std::format("{} uses register {}, which is out of range",
                                              eir::toString(call.kind), argument.value.index()));
                    return false;
                }
                compiled.isConstant = false;
                compiled.location = registerOffsets[argument.value.index()];
                compiled.bitWidth = registerWidths[argument.value.index()];
                break;

            case eir::Operand::Kind::Constant:
                if (argument.value.index() >= out.constants.size()) {
                    reportInvalid(definition, body,
                                  std::format("{} uses constant {}, which is out of range",
                                              eir::toString(call.kind), argument.value.index()));
                    return false;
                }
                compiled.isConstant = true;
                compiled.location = argument.value.index();
                compiled.bitWidth = out.constants[argument.value.index()].bitWidth;
                break;

            default:
                reportInvalid(definition, body,
                              std::format("{} has an argument that is neither a register nor "
                                          "a constant",
                                          eir::toString(call.kind)));
                return false;
            }

            descriptor.arguments.push_back(compiled);
        }

        out.sysCalls.push_back(std::move(descriptor));
    }

    return true;
}

bool Compiler::compileCalls(const eir::ModuleDefinition& definition, const eir::Body& body,
                            const std::vector<std::uint32_t>& registerOffsets,
                            const std::vector<std::uint32_t>& registerWidths, CompiledBody& out) {
    out.calls.reserve(body.calls.size());

    for (const eir::CallSite& call : body.calls) {
        CallDescriptor descriptor;
        descriptor.location = call.location;

        for (const eir::Operand argument : call.arguments) {
            CallDescriptor::Argument compiled;
            if (argument.kind() == eir::Operand::Kind::Register
                && argument.index() < registerOffsets.size()) {
                compiled.location = registerOffsets[argument.index()];
                compiled.bitWidth = registerWidths[argument.index()];
            } else if (argument.kind() == eir::Operand::Kind::Constant
                       && argument.index() < out.constants.size()) {
                compiled.isConstant = true;
                compiled.location = argument.index();
                compiled.bitWidth = out.constants[argument.index()].bitWidth;
            } else {
                reportInvalid(definition, body,
                              std::format("the call of '{}' has an argument that is neither a "
                                          "register nor a constant in range; run the EIR "
                                          "verifier before compiling",
                                          call.subprogram));
                return false;
            }
            descriptor.arguments.push_back(compiled);
        }

        if (call.result.valid()) {
            if (call.result.kind() != eir::Operand::Kind::Register
                || call.result.index() >= registerOffsets.size()) {
                reportInvalid(definition, body,
                              std::format("the call of '{}' returns into something other than a "
                                          "register in range; run the EIR verifier before "
                                          "compiling",
                                          call.subprogram));
                return false;
            }
            descriptor.hasResult = true;
            descriptor.resultLocation = registerOffsets[call.result.index()];
            descriptor.resultWidth = registerWidths[call.result.index()];
        }

        out.calls.push_back(std::move(descriptor));
    }
    return true;
}

void Compiler::reportInvalid(const eir::ModuleDefinition& definition, const eir::Body& body,
                             const std::string& message) {
    diagnostics.report(
        DiagCode::InvalidEir, body.location,
        std::format("module '{}', body '{}': {}", definition.name, body.name, message));
}

} // namespace enzan::codegen
