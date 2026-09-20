// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace enzan::eir {

/// Every EIR-B operation.
///
/// The catalogue, including each opcode's operand convention and its required LLVM
/// lowering sketch, is Opcodes.def. EIR-B is a control-flow graph of three-address
/// instructions over typed values (DESIGN_GUIDE.md section 4.1); it is not the
/// executable form, which `codegen` produces from it.
enum class Opcode : std::uint8_t {
#define ENZAN_OPCODE(name, mnemonic, hasResult, sources, lowering) name,
#include "eir/Opcodes.def"
#undef ENZAN_OPCODE
};

/// Returns the short textual mnemonic, as used when dumping EIR.
std::string_view mnemonic(Opcode opcode) noexcept;

/// True when the opcode writes its destination operand.
bool hasResult(Opcode opcode) noexcept;

/// Number of source operands the opcode uses, in `a`, `b`, `c` order.
std::uint8_t sourceCount(Opcode opcode) noexcept;

/// Returns the recorded sketch of how this opcode lowers to LLVM IR.
///
/// Exposed as an accessor rather than left as a comment so that it can be asserted
/// non-empty by a test: DESIGN_GUIDE.md section 6.1 makes the sketch a requirement
/// for adding an opcode, and a requirement nothing checks is a requirement that
/// erodes.
std::string_view llvmLowering(Opcode opcode) noexcept;

/// True when executing the opcode suspends the process, so that control returns to
/// the scheduler and resumes at the block the instruction names.
bool suspends(Opcode opcode) noexcept;

/// True when the opcode transfers control and therefore must be the last
/// instruction of its basic block.
bool isTerminator(Opcode opcode) noexcept;

/// Which transition on a signal an event control waits for.
enum class EdgeKind : std::uint8_t {
    /// Any value change. This is `@(signal)` and the implicit sensitivity of
    /// `always_comb`.
    AnyChange,
    /// 0, X or Z to 1, or 0 to X or Z. IEEE 1800's definition of `posedge` is a
    /// transition *towards* 1, not merely 0 to 1, which matters whenever a clock
    /// starts out X.
    Posedge,
    /// 1, X or Z to 0, or 1 to X or Z.
    Negedge,
};

std::string_view toString(EdgeKind edge) noexcept;

/// Where an instruction operand lives.
///
/// A single 8-byte tagged index rather than a variant, so that an Instruction stays
/// small and copyable and so that a bytecode lowering is a field-by-field
/// translation.
class Operand {
public:
    enum class Kind : std::uint8_t {
        /// No operand.
        None,
        /// An index into the process's virtual register file.
        Register,
        /// An index into the process's constant pool.
        Constant,
        /// An index into the enclosing module's object table, which elaboration
        /// resolves to a state arena offset.
        Object,
        /// An index into the process's basic block list.
        Block,
        /// An index into the process's system task call descriptors.
        SysCall,
        /// A small literal carried in the instruction, used for enumerations such
        /// as EdgeKind rather than for values.
        Immediate,
    };

    constexpr Operand() noexcept = default;

    static constexpr Operand none() noexcept { return {}; }

    static constexpr Operand reg(std::uint32_t index) noexcept { return {Kind::Register, index}; }

    static constexpr Operand constant(std::uint32_t index) noexcept {
        return {Kind::Constant, index};
    }

    static constexpr Operand object(std::uint32_t index) noexcept { return {Kind::Object, index}; }

    static constexpr Operand block(std::uint32_t index) noexcept { return {Kind::Block, index}; }

    static constexpr Operand sysCall(std::uint32_t index) noexcept {
        return {Kind::SysCall, index};
    }

    static constexpr Operand immediate(std::uint32_t value) noexcept {
        return {Kind::Immediate, value};
    }

    [[nodiscard]] constexpr Kind kind() const noexcept { return operandKind; }

    [[nodiscard]] constexpr bool valid() const noexcept { return operandKind != Kind::None; }

    /// The index or literal this operand carries. Meaningless when `!valid()`.
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return value; }

    friend constexpr bool operator==(Operand, Operand) noexcept = default;

private:
    constexpr Operand(Kind kind, std::uint32_t index) noexcept : operandKind(kind), value(index) {}

    Kind operandKind = Kind::None;
    std::uint32_t value = 0;
};

/// One EIR-B instruction: an opcode, a destination and up to three sources.
///
/// Three-address form throughout, as section 4.1 requires. Operand roles are fixed
/// per opcode and documented in Opcodes.def; nothing infers them from context,
/// because an instruction that means different things in different places cannot be
/// lowered mechanically.
struct Instruction {
    Opcode opcode = Opcode::Nop;

    /// True when the operation treats its operands as signed. Carried on the
    /// instruction rather than derived from the register types, so that the
    /// bytecode is self-contained and a JIT never has to consult the type table.
    bool isSigned = false;

    Operand dst;
    Operand a;
    Operand b;
    Operand c;

    static Instruction make(Opcode opcode, Operand dst = {}, Operand a = {}, Operand b = {},
                            Operand c = {}) {
        return {.opcode = opcode, .isSigned = false, .dst = dst, .a = a, .b = b, .c = c};
    }

    static Instruction makeSigned(Opcode opcode, Operand dst, Operand a, Operand b = {}) {
        return {.opcode = opcode, .isSigned = true, .dst = dst, .a = a, .b = b, .c = {}};
    }
};

} // namespace enzan::eir
