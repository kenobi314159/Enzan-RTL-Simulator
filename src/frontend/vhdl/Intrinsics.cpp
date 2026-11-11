// SPDX-License-Identifier: GPL-3.0-or-later
//
// The operators of std_logic_1164 and numeric_std on vectors, which a design calls for every
// operand of every statement and whose bodies are loops over elements, one table lookup or one
// carry each. A call is made of the same operation on the numbers the vectors are, in a few
// instructions, when every element is a 0, a 1, an L or an H. Any other element leaves what the
// standard says to the body: a metavalue reports itself and picks between 'U' and 'X' there.

#include "frontend/vhdl/BodyTranslator.h"

#include "frontend/vhdl/Types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

using Native = BodyTranslator::Native;
using NativeOp = BodyTranslator::NativeOp;
using NativeParameter = BodyTranslator::NativeParameter;

/// What a parameter of type `type` is to the operators that run natively.
NativeParameter parameterOf(type_t type) {
    if (type_is_array(type)) {
        const std::string_view name = istr(type_ident(type_base_recur(type)));
        if (name == "IEEE.NUMERIC_STD.UNRESOLVED_UNSIGNED" || name == "IEEE.NUMERIC_STD.UNSIGNED")
            return NativeParameter::Unsigned;
        if (name == "IEEE.NUMERIC_STD.UNRESOLVED_SIGNED" || name == "IEEE.NUMERIC_STD.SIGNED")
            return NativeParameter::Signed;
        if (name == "IEEE.STD_LOGIC_1164.STD_LOGIC_VECTOR"
            || name == "IEEE.STD_LOGIC_1164.STD_ULOGIC_VECTOR")
            return NativeParameter::Logic;
        return NativeParameter::Other;
    }
    return type_is_integer(type) ? NativeParameter::Integer : NativeParameter::Other;
}

bool isVector(NativeParameter parameter) {
    return parameter == NativeParameter::Logic || parameter == NativeParameter::Unsigned
           || parameter == NativeParameter::Signed;
}

bool isNumber(NativeParameter parameter) {
    return parameter == NativeParameter::Unsigned || parameter == NativeParameter::Signed;
}

/// The widest vector a call is computed natively for. A body is a loop over the elements, so
/// nothing is lost by leaving the rest to it, and wide arithmetic is the rare design.
constexpr std::int64_t MaxNativeWidth = 4096;

/// What `to_integer` of a vector of `width` elements holds, which is a number only when its
/// widest value is one of the result's type.
constexpr std::uint32_t MaxUnsignedIntegerWidth = 31;
constexpr std::uint32_t MaxSignedIntegerWidth = 32;

} // namespace

std::optional<BodyTranslator::Native> BodyTranslator::nativeOf(tree_t declaration) {
    const std::string_view full = istr(tree_ident2(declaration));
    const bool numeric = full.starts_with("IEEE.NUMERIC_STD.");
    if (!numeric && !full.starts_with("IEEE.STD_LOGIC_1164.")) return std::nullopt;

    Native native;
    native.count = tree_ports(declaration);
    if (native.count < 1 || native.count > 2) return std::nullopt;
    for (unsigned index = 0; index < native.count; ++index)
        native.parameters[index] = parameterOf(tree_type(tree_port(declaration, index)));
    const NativeParameter first = native.parameters[0];
    const NativeParameter second = native.parameters[1];

    std::string_view name = istr(tree_ident(declaration));
    if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);

    // Every operand a number of one signedness: both vectors, or one and an integer.
    const auto signedness = [&]() -> std::optional<bool> {
        if (native.count != 2) return std::nullopt;
        const bool leftNumber = isNumber(first);
        const bool rightNumber = isNumber(second);
        if (leftNumber && rightNumber) {
            if (first != second) return std::nullopt;
            return first == NativeParameter::Signed;
        }
        if (leftNumber && second == NativeParameter::Integer)
            return first == NativeParameter::Signed;
        if (first == NativeParameter::Integer && rightNumber)
            return second == NativeParameter::Signed;
        return std::nullopt;
    };

    struct Logical {
        std::string_view name;
        NativeOp op;
    };

    static constexpr Logical Logicals[] = {{"and", NativeOp::And},   {"or", NativeOp::Or},
                                           {"nand", NativeOp::Nand}, {"nor", NativeOp::Nor},
                                           {"xor", NativeOp::Xor},   {"xnor", NativeOp::Xnor}};
    // On the vectors of one type: a std_logic_vector's operators are std_logic_1164's, and an
    // unsigned's and a signed's numeric_std's.
    const auto sameVectors = [&](unsigned parameters) {
        if (native.count != parameters) return false;
        if (!isVector(first) || (parameters == 2 && second != first)) return false;
        return numeric ? isNumber(first) : first == NativeParameter::Logic;
    };
    for (const Logical& logical : Logicals) {
        if (name == logical.name && sameVectors(2)) {
            native.op = logical.op;
            native.isSigned = first == NativeParameter::Signed;
            return native;
        }
    }
    if (name == "not" && sameVectors(1)) {
        native.op = NativeOp::Not;
        native.isSigned = first == NativeParameter::Signed;
        return native;
    }
    if (!numeric) return std::nullopt;

    if (name == "-" && native.count == 1 && first == NativeParameter::Signed) {
        native.op = NativeOp::Negate;
        native.isSigned = true;
        return native;
    }
    if (name == "TO_INTEGER" && native.count == 1 && isNumber(first)) {
        native.op = NativeOp::ToInteger;
        native.isSigned = first == NativeParameter::Signed;
        return native;
    }
    if ((name == "TO_UNSIGNED" || name == "TO_SIGNED") && native.count == 2
        && first == NativeParameter::Integer && second == NativeParameter::Integer) {
        native.op = name == "TO_SIGNED" ? NativeOp::ToSigned : NativeOp::ToUnsigned;
        native.isSigned = name == "TO_SIGNED";
        return native;
    }
    if ((name == "RESIZE" || name == "SHIFT_LEFT" || name == "SHIFT_RIGHT") && native.count == 2
        && isNumber(first) && second == NativeParameter::Integer) {
        native.op = NativeOp::ShiftRight;
        if (name == "RESIZE") native.op = NativeOp::Resize;
        if (name == "SHIFT_LEFT") native.op = NativeOp::ShiftLeft;
        native.isSigned = first == NativeParameter::Signed;
        return native;
    }

    struct Arithmetic {
        std::string_view name;
        NativeOp op;
    };

    static constexpr Arithmetic Operators[] = {
        {"+", NativeOp::Add},        {"-", NativeOp::Subtract},  {"*", NativeOp::Multiply},
        {"=", NativeOp::Equal},      {"/=", NativeOp::NotEqual}, {"<", NativeOp::Less},
        {"<=", NativeOp::LessEqual}, {">", NativeOp::Greater},   {">=", NativeOp::GreaterEqual}};
    for (const Arithmetic& arithmetic : Operators) {
        if (name != arithmetic.name) continue;
        const std::optional<bool> isSigned = signedness();
        if (!isSigned) return std::nullopt;
        native.op = arithmetic.op;
        native.isSigned = *isSigned;
        return native;
    }
    return std::nullopt;
}

BodyTranslator::NativeResult BodyTranslator::nativeCall(tree_t call, tree_t declaration,
                                                        const std::vector<tree_t>& actuals,
                                                        const Native& native) {
    const bool isLogical = native.op <= NativeOp::Not;

    // What a vector actual has to be for the call to be computed here: one whose length is known
    // when this is translated.
    for (unsigned index = 0; index < native.count; ++index) {
        tree_t formal = tree_port(declaration, index);
        if (isVector(native.parameters[index])) {
            if (layoutFor(tree_type(formal))) return {};
            if (passesInScratch(actuals[index], tree_type(formal))
                || runTimeString(actuals[index]) != nullptr || isRunTimeSlice(actuals[index]))
                return {};
            // A body of its own takes no null array, which has no bits to pass: the call is
            // inlined, as any is.
            if (const std::optional<Folder::Range> bounds = folder().boundsOf(actuals[index]);
                bounds && bounds->length() == 0)
                return {};
        }
    }
    // A size is a number of elements the result is made of, which a body takes a shape for.
    std::int64_t size = 0;
    if (native.op == NativeOp::ToUnsigned || native.op == NativeOp::ToSigned
        || native.op == NativeOp::Resize) {
        const std::optional<std::int64_t> folded = folder().integer(actuals[1]);
        if (!folded || *folded < 1 || *folded > MaxNativeWidth) return {};
        size = *folded;
    }

    // The operands, evaluated once.
    Shape shape;
    std::vector<Value> values;
    for (unsigned index = 0; index < native.count; ++index) {
        tree_t formal = tree_port(declaration, index);
        shape.runTimeStrings.emplace_back();
        const std::optional<Value> value = expression(actuals[index]);
        if (!value) return {.handled = true, .value = std::nullopt};
        const bool vector = isVector(native.parameters[index]);
        if (vector && !value->layout.isArray()) {
            fail(actuals[index], "an actual whose bounds are not known here");
            return {.handled = true, .value = std::nullopt};
        }
        shape.bounds.push_back(vector ? std::optional(value->layout.index) : std::nullopt);
        shape.elementBounds.push_back(vector ? elementBoundsOf(tree_type(formal), value->layout)
                                             : std::nullopt);
        shape.values.push_back(vector ? std::nullopt : folder().integer(actuals[index]));
        values.push_back(*value);
    }

    // Lengths the code below is made for: a vector of an element a byte wide, and as many as
    // can be a number of the width.
    std::array<std::int64_t, 2> widths = {0, 0};
    bool usable = true;
    for (unsigned index = 0; index < native.count; ++index) {
        if (!isVector(native.parameters[index])) continue;
        const Layout& layout = values[index].layout;
        widths[index] = layout.index.length();
        usable = usable && layout.elementBits == 8 && !layout.runTimeString && widths[index] >= 1
                 && widths[index] <= MaxNativeWidth
                 && values[index].type.bitWidth() == 8 * widths[index];
    }
    if (isLogical && native.count == 2 && widths[0] != widths[1]) usable = false;
    if (native.op == NativeOp::ToInteger
        && widths[0] > (native.isSigned ? MaxSignedIntegerWidth : MaxUnsignedIntegerWidth))
        usable = false;
    if (!usable) {
        return {.handled = true,
                .value = emitCallSite(call, declaration, actuals, shape, values, {})};
    }

    const eir::Encoding logicCodes = eir::Encoding::VhdlLogic;
    const bool isSigned = native.isSigned;
    const auto fourState = [](std::uint32_t width, bool sign) {
        return eir::Type::bits(width, sign, true);
    };

    // `value`'s width as the bits it is, of the signedness `sign` says, in a register of its own
    // when that differs from its width now.
    const auto extend = [&](const Value& value, std::uint32_t width, bool sign) -> Value {
        const std::uint32_t from = value.type.bitWidth();
        if (from == width && value.type.isSigned() == sign) return value;
        const eir::Type type = fourState(width, sign);
        if (from == width) return Value{.reg = value.reg, .type = type};
        const std::uint32_t reg = newRegister(type);
        const eir::Opcode opcode = width > from ? eir::Opcode::Extend : eir::Opcode::Truncate;
        // The signedness of an extension is the source's, however the destination is typed.
        emit(sign ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(reg),
                                                 eir::Operand::reg(value.reg))
                  : eir::Instruction::make(opcode, eir::Operand::reg(reg),
                                           eir::Operand::reg(value.reg)));
        return Value{.reg = reg, .type = type};
    };

    // A vector of codes as the bits they stand for.
    const auto present = [&](const Value& vector, bool sign) -> Value {
        const eir::Type type = fourState(vector.type.bitWidth() / 8, sign);
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(
            eir::Opcode::Present, eir::Operand::reg(reg), eir::Operand::reg(vector.reg),
            eir::Operand::immediate(static_cast<std::uint32_t>(logicCodes))));
        return Value{.reg = reg, .type = type};
    };

    const auto binary = [&](eir::Opcode opcode, const Value& left, const Value& right,
                            std::uint32_t width, bool sign) -> Value {
        const eir::Type type = fourState(width, sign);
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(opcode, eir::Operand::reg(reg), eir::Operand::reg(left.reg),
                                    eir::Operand::reg(right.reg)));
        return Value{.reg = reg, .type = type};
    };

    const auto bit = [&] { return eir::Type::bits(1, false, true); };

    // One bit, 1 when `value` has no unknown bit, and unknown when it has: a value equals itself
    // only when it is known.
    const auto known = [&](const Value& value) -> std::uint32_t {
        const std::uint32_t reg = newRegister(bit());
        emit(eir::Instruction::make(eir::Opcode::Equal, eir::Operand::reg(reg),
                                    eir::Operand::reg(value.reg), eir::Operand::reg(value.reg)));
        return reg;
    };

    // A condition of two, true when both are; the unknown of either is false to a branch.
    const auto both = [&](std::uint32_t left, std::uint32_t right) -> std::uint32_t {
        const std::uint32_t reg = newRegister(bit());
        emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(reg),
                                    eir::Operand::reg(left), eir::Operand::reg(right)));
        return reg;
    };

    // An integer operand as a number of `width` bits and the signedness the operator has, which
    // is how `to_unsigned` and `to_signed` take it: truncated or extended.
    const auto number = [&](const Value& integer, std::uint32_t width) -> Value {
        const Value typed{.reg = integer.reg, .type = fourState(integer.type.bitWidth(), isSigned)};
        return extend(typed, width, isSigned);
    };

    // Whether an integer fits in `width` bits as `to_unsigned` or `to_signed` makes them: a
    // truncation is reported by the body.
    const auto fits = [&](const Value& integer, std::uint32_t width) -> std::uint32_t {
        const std::uint32_t integerWidth = integer.type.bitWidth();
        const auto compare = [&](eir::Opcode opcode, std::int64_t limit, bool sign, bool swapped) {
            const Value bound =
                constant(limit, eir::Type::bits(integerWidth, sign, false), nullptr);
            const std::uint32_t reg = newRegister(bit());
            const std::uint32_t left = swapped ? bound.reg : integer.reg;
            const std::uint32_t right = swapped ? integer.reg : bound.reg;
            emit(sign ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(reg),
                                                     eir::Operand::reg(left),
                                                     eir::Operand::reg(right))
                      : eir::Instruction::make(opcode, eir::Operand::reg(reg),
                                               eir::Operand::reg(left), eir::Operand::reg(right)));
            return reg;
        };
        if (!isSigned) {
            // Below 2**width, which a negative number, read as an unsigned one, is not.
            const std::int64_t limit = width >= integerWidth - 1
                                           ? std::int64_t{1} << (integerWidth - 1)
                                           : std::int64_t{1} << width;
            return compare(eir::Opcode::LessThan, limit, false, false);
        }
        if (width >= integerWidth) {
            // Every integer fits; a register that is known to be is still a condition.
            return known(integer);
        }
        const std::int64_t limit = std::int64_t{1} << (width - 1);
        const std::uint32_t above = compare(eir::Opcode::LessEqual, -limit, true, true);
        const std::uint32_t below = compare(eir::Opcode::LessThan, limit, true, false);
        return both(above, below);
    };

    // --- the code of each operator, and the call of its body for the rest ---------------------

    // What the fast path computes up to the test that says it may be used, and what it then does
    // to put the result in its register.
    std::uint32_t condition = 0;
    std::function<void(std::uint32_t)> finish;

    const auto fromBits = [&](const Value& bits) {
        // Bits that are known, as the codes of the vector they are.
        return [&, bits](std::uint32_t result) {
            emit(eir::Instruction::make(
                eir::Opcode::Encode, eir::Operand::reg(result), eir::Operand::reg(bits.reg),
                eir::Operand::immediate(static_cast<std::uint32_t>(logicCodes)),
                eir::Operand::immediate(0)));
        };
    };

    const auto leftWidth = static_cast<std::uint32_t>(widths[0]);
    const auto rightWidth = static_cast<std::uint32_t>(widths[1]);
    switch (native.op) {
    case NativeOp::And:
    case NativeOp::Or:
    case NativeOp::Nand:
    case NativeOp::Nor:
    case NativeOp::Xor:
    case NativeOp::Xnor: {
        const Value left = present(values[0], false);
        const Value right = present(values[1], false);
        const bool inverted = native.op == NativeOp::Nand || native.op == NativeOp::Nor;
        eir::Opcode opcode = eir::Opcode::And;
        if (native.op == NativeOp::Or || native.op == NativeOp::Nor) opcode = eir::Opcode::Or;
        if (native.op == NativeOp::Xor) opcode = eir::Opcode::Xor;
        if (native.op == NativeOp::Xnor) opcode = eir::Opcode::Xnor;
        Value result = binary(opcode, left, right, leftWidth, false);
        if (inverted) {
            const std::uint32_t reg = newRegister(result.type);
            emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(reg),
                                        eir::Operand::reg(result.reg)));
            result.reg = reg;
        }
        // A known result is the same whatever the 'U' and 'X' among the operands would have made
        // of another, and a 0 or a 1 of two operands that decide it is what the tables give.
        condition = known(result);
        finish = fromBits(result);
        break;
    }
    case NativeOp::Not: {
        const Value operand = present(values[0], false);
        const std::uint32_t reg = newRegister(operand.type);
        emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(reg),
                                    eir::Operand::reg(operand.reg)));
        const Value result{.reg = reg, .type = operand.type};
        condition = known(result);
        finish = fromBits(result);
        break;
    }
    case NativeOp::Negate: {
        const Value operand = present(values[0], true);
        const std::uint32_t reg = newRegister(operand.type);
        emit(eir::Instruction::make(eir::Opcode::Negate, eir::Operand::reg(reg),
                                    eir::Operand::reg(operand.reg)));
        const Value result{.reg = reg, .type = operand.type};
        condition = known(result);
        finish = fromBits(result);
        break;
    }
    case NativeOp::Add:
    case NativeOp::Subtract:
    case NativeOp::Multiply: {
        const bool integerLeft = native.parameters[0] == NativeParameter::Integer;
        const bool integerRight = native.parameters[1] == NativeParameter::Integer;
        // An operand and a number are as wide as the operand; a product is as wide as both.
        const std::uint32_t operandWidth = integerLeft ? rightWidth : leftWidth;
        const std::uint32_t sumWidth =
            integerLeft || integerRight ? operandWidth : std::max(leftWidth, rightWidth);
        const bool product = native.op == NativeOp::Multiply;
        std::uint32_t width = sumWidth;
        if (product)
            width = integerLeft || integerRight ? 2 * operandWidth : leftWidth + rightWidth;
        std::optional<std::uint32_t> inRange;
        const auto operand = [&](unsigned index) -> Value {
            if (native.parameters[index] == NativeParameter::Integer) {
                inRange = inRange ? both(*inRange, fits(values[index], operandWidth))
                                  : fits(values[index], operandWidth);
                return number(values[index], width);
            }
            return extend(present(values[index], isSigned), width, isSigned);
        };
        const Value left = operand(0);
        const Value right = operand(1);
        eir::Opcode opcode = eir::Opcode::Multiply;
        if (native.op == NativeOp::Add) opcode = eir::Opcode::Add;
        if (native.op == NativeOp::Subtract) opcode = eir::Opcode::Subtract;
        const Value result = binary(opcode, left, right, width, isSigned);
        condition = known(result);
        if (inRange) condition = both(condition, *inRange);
        finish = fromBits(result);
        break;
    }
    case NativeOp::Equal:
    case NativeOp::NotEqual:
    case NativeOp::Less:
    case NativeOp::LessEqual:
    case NativeOp::Greater:
    case NativeOp::GreaterEqual: {
        // The numbers compared, each as wide as the wider: an integer is thirty-two bits.
        std::uint32_t width = 0;
        for (unsigned index = 0; index < 2; ++index) {
            width = std::max(width, native.parameters[index] == NativeParameter::Integer
                                        ? values[index].type.bitWidth()
                                        : static_cast<std::uint32_t>(widths[index]));
        }
        std::optional<std::uint32_t> ready;
        const auto operand = [&](unsigned index) -> Value {
            if (native.parameters[index] == NativeParameter::Integer) {
                // A natural is no less than 0.
                if (!isSigned) {
                    const Value zero = constant(0, values[index].type, nullptr);
                    const std::uint32_t reg = newRegister(bit());
                    emit(eir::Instruction::makeSigned(
                        eir::Opcode::LessEqual, eir::Operand::reg(reg), eir::Operand::reg(zero.reg),
                        eir::Operand::reg(values[index].reg)));
                    ready = ready ? both(*ready, reg) : reg;
                }
                return number(values[index], width);
            }
            const Value bits = present(values[index], isSigned);
            const std::uint32_t reg = known(bits);
            ready = ready ? both(*ready, reg) : reg;
            return extend(bits, width, isSigned);
        };
        const Value left = operand(0);
        const Value right = operand(1);
        condition = *ready;
        const bool swapped = native.op == NativeOp::Greater || native.op == NativeOp::GreaterEqual;
        eir::Opcode opcode = eir::Opcode::LessEqual;
        if (native.op == NativeOp::Equal) opcode = eir::Opcode::Equal;
        if (native.op == NativeOp::NotEqual) opcode = eir::Opcode::NotEqual;
        if (native.op == NativeOp::Less || native.op == NativeOp::Greater)
            opcode = eir::Opcode::LessThan;
        finish = [&, left, right, swapped, opcode](std::uint32_t result) {
            const std::uint32_t test = newRegister(bit());
            const std::uint32_t first = swapped ? right.reg : left.reg;
            const std::uint32_t last = swapped ? left.reg : right.reg;
            emit(isSigned
                     ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(test),
                                                    eir::Operand::reg(first),
                                                    eir::Operand::reg(last))
                     : eir::Instruction::make(opcode, eir::Operand::reg(test),
                                              eir::Operand::reg(first), eir::Operand::reg(last)));
            emit(eir::Instruction::make(eir::Opcode::Extend, eir::Operand::reg(result),
                                        eir::Operand::reg(test)));
        };
        break;
    }
    case NativeOp::ToInteger: {
        const Value bits = present(values[0], isSigned);
        condition = known(bits);
        finish = [&, bits](std::uint32_t result) {
            // Into the result's own type, whose width is no less than the vector's.
            const std::uint32_t width = body().registerTypes[result].bitWidth();
            const eir::Opcode opcode =
                width == bits.type.bitWidth() ? eir::Opcode::Move : eir::Opcode::Extend;
            emit(isSigned && opcode == eir::Opcode::Extend
                     ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(result),
                                                    eir::Operand::reg(bits.reg))
                     : eir::Instruction::make(opcode, eir::Operand::reg(result),
                                              eir::Operand::reg(bits.reg)));
        };
        break;
    }
    case NativeOp::ToUnsigned:
    case NativeOp::ToSigned: {
        const auto width = static_cast<std::uint32_t>(size);
        condition = fits(values[0], width);
        const Value bits = number(values[0], width);
        finish = fromBits(bits);
        break;
    }
    case NativeOp::ShiftLeft:
    case NativeOp::ShiftRight:
        return shiftNative(call, values[0], values[1], native.op == NativeOp::ShiftLeft, isSigned);
    case NativeOp::Resize: return resizeNative(call, values[0], size, isSigned);
    }

    // --- the two ways, and where they meet --------------------------------------------------

    const std::uint32_t fast = newBlock("native");
    const std::uint32_t slow = newBlock("native-body");
    const std::uint32_t join = newBlock("native-join");
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition),
                                eir::Operand::block(fast), eir::Operand::block(slow)));

    selectBlock(slow);
    const std::optional<Value> bodyCall =
        emitCallSite(call, declaration, actuals, shape, values, {});
    if (!bodyCall) return {.handled = true, .value = std::nullopt};
    const std::uint32_t result = newRegister(bodyCall->type);
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(result),
                                eir::Operand::reg(bodyCall->reg)));
    jump(join);

    selectBlock(fast);
    finish(result);
    jump(join);

    selectBlock(join);
    Value value = *bodyCall;
    value.reg = result;
    return {.handled = true, .value = value};
}

BodyTranslator::NativeResult BodyTranslator::shiftNative(tree_t call, const Value& vector,
                                                         const Value& count, bool left,
                                                         bool isSigned) {
    // The elements are moved as they are, a metavalue and an L or an H among them, so there is no
    // body to leave anything to. The vector is shifted as the bits its codes are, a count of
    // elements being eight bits each, and what the shift leaves vacant is filled with the element
    // that comes in: a 0, whose code is 2, or a signed number's sign element on the right.
    const std::uint32_t bits = vector.type.bitWidth();
    const std::uint32_t elements = bits / 8;
    const eir::Type type = eir::Type::bits(bits, false, false);
    Layout layout = vector.layout;
    layout.storage = type;
    layout.index = Folder::Range{
        .left = static_cast<std::int64_t>(elements) - 1, .right = 0, .ascending = false};

    const eir::Type wide = eir::Type::bits(64, false, false);
    const std::uint32_t amount = newRegister(wide);
    emit(eir::Instruction::make(eir::Opcode::Extend, eir::Operand::reg(amount),
                                eir::Operand::reg(count.reg)));
    const std::uint32_t scaled = newRegister(wide);
    emit(eir::Instruction::make(eir::Opcode::Multiply, eir::Operand::reg(scaled),
                                eir::Operand::reg(amount),
                                eir::Operand::reg(constant(8, wide, nullptr).reg)));

    const eir::Opcode direction = left ? eir::Opcode::ShiftLeft : eir::Opcode::ShiftRightLogical;
    const auto shift = [&](std::uint32_t operand) {
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(direction, eir::Operand::reg(reg), eir::Operand::reg(operand),
                                    eir::Operand::reg(scaled)));
        return reg;
    };
    const auto bitwise = [&](eir::Opcode opcode, std::uint32_t first, std::uint32_t second) {
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(opcode, eir::Operand::reg(reg), eir::Operand::reg(first),
                                    eir::Operand::reg(second)));
        return reg;
    };

    // The bits the shift brings in, a 1 for each, and so the ones to fill.
    const Value ones =
        constantArray(eir::BitVector::filled(bits, eir::Logic::One), layout, nullptr);
    const std::uint32_t vacated = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(vacated),
                                eir::Operand::reg(shift(ones.reg))));

    std::uint32_t fill = 0;
    if (!left && isSigned) {
        const std::uint32_t sign = newRegister(eir::Type::bits(8, false, false));
        emit(eir::Instruction::make(
            eir::Opcode::PartSelect, eir::Operand::reg(sign), eir::Operand::reg(vector.reg),
            eir::Operand::reg(constant(bits - 8, eir::Type::bits(32, false, false), nullptr).reg)));
        const std::uint32_t signWide = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::Extend, eir::Operand::reg(signWide),
                                    eir::Operand::reg(sign)));
        eir::BitVector bytes = eir::BitVector::filled(bits, eir::Logic::Zero);
        for (std::uint32_t element = 0; element < elements; ++element)
            bytes.set(8 * element, eir::Logic::One);
        fill = bitwise(eir::Opcode::Multiply, signWide, constantArray(bytes, layout, nullptr).reg);
    } else {
        // A 0 of std_ulogic is code 2.
        eir::BitVector zeros = eir::BitVector::filled(bits, eir::Logic::Zero);
        for (std::uint32_t element = 0; element < elements; ++element)
            zeros.set((8 * element) + 1, eir::Logic::One);
        fill = constantArray(zeros, layout, nullptr).reg;
    }
    const std::uint32_t result =
        bitwise(eir::Opcode::Or, shift(vector.reg), bitwise(eir::Opcode::And, fill, vacated));
    return {.handled = true,
            .value = Value{.reg = result,
                           .type = type,
                           .vhdlType = tree_has_type(call) ? tree_type(call) : nullptr,
                           .layout = layout}};
}

BodyTranslator::NativeResult BodyTranslator::resizeNative(tree_t call, const Value& vector,
                                                          std::int64_t size, bool isSigned) {
    // Elements are copied as they are, a metavalue among them too, so there is no body to leave
    // anything to: a vector of `size` elements, the right-hand ones of the argument's, and the
    // rest the 0 an unsigned is extended with or the sign element a signed one is.
    const auto width = static_cast<std::uint32_t>(size);
    const std::uint32_t from = vector.type.bitWidth() / 8;
    const eir::Type type = eir::Type::bits(8 * width, false, false);
    Layout layout = vector.layout;
    layout.storage = type;
    layout.index = Folder::Range{.left = size - 1, .right = 0, .ascending = false};

    const eir::Type offsetType = eir::Type::bits(32, false, false);
    const auto at = [&](std::int64_t offset) { return constant(offset, offsetType, nullptr).reg; };
    const auto select = [&](const Value& whole, std::int64_t offset, std::uint32_t bits) {
        const std::uint32_t reg = newRegister(eir::Type::bits(bits, false, false));
        emit(eir::Instruction::make(eir::Opcode::PartSelect, eir::Operand::reg(reg),
                                    eir::Operand::reg(whole.reg), eir::Operand::reg(at(offset))));
        return reg;
    };
    const auto splice = [&](std::uint32_t into, std::uint32_t piece, std::int64_t offset) {
        emit(eir::Instruction::make(eir::Opcode::SplicePart, eir::Operand::reg(into),
                                    eir::Operand::reg(piece), eir::Operand::reg(at(offset))));
    };

    std::uint32_t result = vector.reg;
    if (width < from) {
        result = select(vector, 0, 8 * width);
        // The sign element of a signed number stays where it is, over the elements that remain.
        if (isSigned)
            splice(result, select(vector, 8 * (static_cast<std::int64_t>(from) - 1), 8),
                   8 * (static_cast<std::int64_t>(width) - 1));
    } else if (width > from) {
        if (isSigned) {
            // The sign element, copied into every element above the argument's by a product with
            // a 1 in each of those bytes, where no sum carries.
            const std::uint32_t sign = select(vector, 8 * (static_cast<std::int64_t>(from) - 1), 8);
            const std::uint32_t wide = newRegister(type);
            emit(eir::Instruction::make(eir::Opcode::Extend, eir::Operand::reg(wide),
                                        eir::Operand::reg(sign)));
            eir::BitVector ones = eir::BitVector::filled(8 * width, eir::Logic::Zero);
            for (std::uint32_t element = from; element < width; ++element)
                ones.set(8 * element, eir::Logic::One);
            const Value factor = constantArray(ones, layout, nullptr);
            result = newRegister(type);
            emit(eir::Instruction::make(eir::Opcode::Multiply, eir::Operand::reg(result),
                                        eir::Operand::reg(wide), eir::Operand::reg(factor.reg)));
        } else {
            // A 0 of std_ulogic is code 2.
            eir::BitVector zeros = eir::BitVector::filled(8 * width, eir::Logic::Zero);
            for (std::uint32_t element = from; element < width; ++element)
                zeros.set((8 * element) + 1, eir::Logic::One);
            result = constantArray(zeros, layout, nullptr).reg;
        }
        splice(result, vector.reg, 0);
    }
    return {.handled = true,
            .value = Value{.reg = result,
                           .type = type,
                           .vhdlType = tree_has_type(call) ? tree_type(call) : nullptr,
                           .layout = layout}};
}

} // namespace enzan::frontend::vhdl
