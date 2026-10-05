// SPDX-License-Identifier: GPL-3.0-or-later

#include "codegen/Compiler.h"

#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "eir/Module.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace enzan::codegen {

std::uint32_t CompiledDesign::bodyIndex(std::uint32_t definitionIndex,
                                        std::uint32_t processIndex) const {
    const auto found = std::ranges::find_if(keys, [&](const Key& key) {
        return key.definitionIndex == definitionIndex && key.processIndex == processIndex;
    });
    return found == keys.end() ? NotCompiled : found->bodyIndex;
}

namespace {

/// True for the four opcodes that write an object, which are the ones the runtime routes.
bool isStore(eir::Opcode opcode) noexcept {
    return opcode == eir::Opcode::Store || opcode == eir::Opcode::StorePart
           || opcode == eir::Opcode::StoreNonBlocking
           || opcode == eir::Opcode::StorePartNonBlocking;
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
            CompiledProcess body;
            if (!compileProcess(definition, definition.processes[processIndex], body))
                return nullptr;

            const auto compiledIndex = static_cast<std::uint32_t>(result->processes.size());
            result->processes.push_back(std::move(body));
            result->keys.push_back(CompiledDesign::Key{.definitionIndex = definitionIndex,
                                                       .processIndex = processIndex,
                                                       .bodyIndex = compiledIndex});
        }
    }

    return result;
}

bool Compiler::compileProcess(const eir::ModuleDefinition& definition, const eir::Process& process,
                              CompiledProcess& out) {
    out.kind = process.kind;
    out.name = process.name;
    out.location = process.location;
    out.sensitivity = process.sensitivity;
    out.readSet = process.readSet;
    out.writeSet = process.writeSet;
    out.startsAtTimeZero = eir::startsAtTimeZero(process);
    out.strings = process.strings;

    Frame frame;
    frame.build(process.registerTypes);
    out.frameWords = frame.words;

    // Constants are laid out end to end in the same two-plane form the arena uses, so
    // that reading one is a view over the pool rather than a copy.
    out.constants.reserve(process.constants.size());
    for (const eir::BitVector& value : process.constants) {
        const auto offset = static_cast<std::uint32_t>(out.constantPool.size());
        out.constants.push_back(Constant{.wordOffset = offset, .bitWidth = value.width()});

        const eir::ConstBitRef source = value.ref();
        out.constantPool.insert(out.constantPool.end(), source.avalWords().begin(),
                                source.avalWords().end());
        out.constantPool.insert(out.constantPool.end(), source.bvalWords().begin(),
                                source.bvalWords().end());
    }

    if (!compileSysTaskCalls(definition, process, frame.offsets, frame.widths, out)) return false;

    std::vector<Fixup> fixups;
    out.blockStarts.reserve(process.blocks.size());

    const std::vector<std::uint32_t> written = eir::writtenObjects(process.writeSet);

    for (const eir::BasicBlock& block : process.blocks) {
        out.blockStarts.push_back(static_cast<std::uint32_t>(out.code.size()));

        for (const eir::Instruction& source : block.instructions) {
            Instruction emitted;
            emitted.opcode = source.opcode;
            emitted.isSigned = source.isSigned;

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
                    reportInvalid(definition, process,
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
                    reportInvalid(definition, process,
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
                if (sources[index].index() >= process.blocks.size()) {
                    reportInvalid(definition, process,
                                  std::format("branch to block {}, but the process has {}",
                                              sources[index].index(), process.blocks.size()));
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

bool Compiler::compileSysTaskCalls(const eir::ModuleDefinition& definition,
                                   const eir::Process& process,
                                   const std::vector<std::uint32_t>& registerOffsets,
                                   const std::vector<std::uint32_t>& registerWidths,
                                   CompiledProcess& out) {
    out.sysCalls.reserve(process.sysCalls.size());

    for (const eir::SysTaskCall& call : process.sysCalls) {
        SysTaskDescriptor descriptor;
        descriptor.kind = call.kind;
        descriptor.formatIndex = call.formatIndex;
        descriptor.location = call.location;

        if (call.hasFormat() && call.formatIndex >= process.strings.size()) {
            reportInvalid(definition, process,
                          std::format("{} names string {}, but the process has {}",
                                      eir::toString(call.kind), call.formatIndex,
                                      process.strings.size()));
            return false;
        }

        for (const eir::SysTaskArgument& argument : call.arguments) {
            SysTaskDescriptor::Argument compiled;
            compiled.isSigned = argument.type.isSigned();
            compiled.bitWidth = argument.type.bitWidth();

            switch (argument.value.kind()) {
            case eir::Operand::Kind::Register:
                if (argument.value.index() >= registerOffsets.size()) {
                    reportInvalid(definition, process,
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
                    reportInvalid(definition, process,
                                  std::format("{} uses constant {}, which is out of range",
                                              eir::toString(call.kind), argument.value.index()));
                    return false;
                }
                compiled.isConstant = true;
                compiled.location = argument.value.index();
                compiled.bitWidth = out.constants[argument.value.index()].bitWidth;
                break;

            default:
                reportInvalid(definition, process,
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

void Compiler::reportInvalid(const eir::ModuleDefinition& definition, const eir::Process& process,
                             const std::string& message) {
    diagnostics.report(
        DiagCode::InvalidEir, process.location,
        std::format("module '{}', process '{}': {}", definition.name, process.name, message));
}

} // namespace enzan::codegen
