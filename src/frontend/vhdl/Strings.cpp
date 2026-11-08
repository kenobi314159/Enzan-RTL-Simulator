// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strings whose lengths only the run knows, held in the kernel's scratch (DESIGN_GUIDE.md section
// 4.1).
//
// A register of EIR has a width fixed before the run, so a subprogram's `string(1 to n)` cannot be
// one. It is a string the kernel holds instead, and a register holds its handle: what reads its
// bounds or its characters, writes them, or compares, reads or writes it as textio does, is a
// system function the runtime runs over it. A subprogram's strings are released as it returns.

#include "frontend/vhdl/BodyTranslator.h"
#include "frontend/vhdl/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace enzan::frontend::vhdl {

namespace {

const eir::Type Handle = eir::Type::bits(64, false, false);
const eir::Type Index = eir::Type::bits(64, true, false);
const eir::Type Character = eir::Type::bits(8, false, false);

eir::SysTaskArgument argumentOf(std::uint32_t reg, eir::Type type) {
    return eir::SysTaskArgument{.value = eir::Operand::reg(reg), .type = type};
}

} // namespace

const BodyTranslator::Value* BodyTranslator::runTimeString(tree_t name) const {
    if (tree_kind(name) != T_REF || !tree_has_ref(name)) return nullptr;
    const auto local = locals.find(tree_ref(name));
    return local != locals.end() && local->second.layout.runTimeString ? &local->second : nullptr;
}

const BodyTranslator::Value* BodyTranslator::runTimeStringRange(tree_t range) const {
    if (tree_subkind(range) != RANGE_EXPR || !tree_has_value(range)) return nullptr;
    tree_t attribute = tree_value(range);
    if (tree_kind(attribute) != T_ATTR_REF
        || (tree_subkind(attribute) != ATTR_RANGE && tree_subkind(attribute) != ATTR_REVERSE_RANGE))
        return nullptr;
    return runTimeString(tree_name(attribute));
}

BodyTranslator::Value BodyTranslator::scratchMark() {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringMark;
    call.location = statementLocation;
    return systemFunction(std::move(call), Handle);
}

void BodyTranslator::releaseScratch(const Value& mark) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringRelease;
    call.location = statementLocation;
    call.arguments.push_back(argumentOf(mark.reg, Handle));
    emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                eir::Operand::sysCall(addSysCall(std::move(call)))));
}

std::optional<bool> BodyTranslator::declareRunTimeString(tree_t declaration) {
    // A process's variables last as long as it does, past every suspension; a subprogram's are
    // made anew at each call.
    type_t type = tree_type(declaration);
    if (!isStringType(type) || (isProcess && inlined.empty())) return std::nullopt;

    // The nearest index constraint among the subtypes it is declared through. One of an
    // unconstrained subtype takes its bounds from its value, which is not held yet.
    type_t constrained = type;
    while (type_kind(constrained) == T_SUBTYPE && !type_has_constraint(constrained))
        constrained = type_base(constrained);
    if (type_kind(constrained) != T_SUBTYPE) return std::nullopt;
    tree_t range = range_of(constrained, 0);

    Value left;
    Value right;
    bool ascending = true;
    switch (tree_subkind(range)) {
    case RANGE_TO:
    case RANGE_DOWNTO: {
        ascending = tree_subkind(range) == RANGE_TO;
        const std::optional<Value> from = expression(tree_left(range));
        const std::optional<Value> to = expression(tree_right(range));
        if (!from || !to) return false;
        left = resize(*from, 64);
        right = resize(*to, 64);
        break;
    }

    // `s'range` or `s'reverse_range` of another string of run-time length.
    case RANGE_EXPR: {
        const Value* other = runTimeStringRange(range);
        if (other == nullptr) return std::nullopt;
        const StringBounds bounds = stringBounds(*other, declaration);
        const bool reversed = tree_subkind(tree_value(range)) == ATTR_REVERSE_RANGE;
        ascending = other->layout.index.ascending != reversed;
        left = reversed ? bounds.right : bounds.left;
        right = reversed ? bounds.left : bounds.right;
        break;
    }
    default: return std::nullopt;
    }

    // Each character starts as `character`'s left, NUL, or as an aggregate of one fills it.
    Value fill = constant(0, Character, nullptr);
    if (tree_has_value(declaration)) {
        tree_t value = tree_value(declaration);
        if (tree_kind(value) != T_AGGREGATE || tree_assocs(value) != 1
            || tree_subkind(tree_assoc(value, 0)) != A_OTHERS)
            return fail(declaration, "an initial value of a string of run-time length other than "
                                     "an aggregate of one character");
        const std::optional<Value> character = expression(tree_value(tree_assoc(value, 0)));
        if (!character) return false;
        fill = resize(*character, 8);
    }

    // Released as the subprogram returns: an inlined one's frame holds the mark, and a wait
    // while it does would release what it holds.
    std::optional<Value>& mark = inlined.empty() ? ownScratchMark : inlined.back().scratchMark;
    if (!mark) {
        mark = scratchMark();
        if (!inlined.empty()) ++scratchHeld;
    }

    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringNew;
    call.location = owner.locate(declaration);
    const Value up = constant(ascending ? 1 : 0, eir::Type::bits(1, false, false), nullptr);
    call.arguments = {argumentOf(left.reg, Index), argumentOf(right.reg, Index),
                      argumentOf(up.reg, up.type), argumentOf(fill.reg, Character)};
    const Value made = systemFunction(std::move(call), Handle);
    locals[declaration] = Value{.reg = made.reg,
                                .type = Handle,
                                .vhdlType = type,
                                .layout = runTimeStringLayout(type, ascending)};
    return true;
}

BodyTranslator::StringBounds BodyTranslator::stringBounds(const Value& string, tree_t where) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringBounds;
    call.location = owner.locate(where);
    call.arguments.push_back(argumentOf(string.reg, Handle));
    const Value bounds = systemFunction(std::move(call), eir::Type::bits(192, false, false));
    const auto part = [&](std::uint32_t lowBit) {
        const Value result{.reg = newRegister(Index), .type = Index, .vhdlType = nullptr};
        emit(eir::Instruction::make(
            eir::Opcode::PartSelect, eir::Operand::reg(result.reg), eir::Operand::reg(bounds.reg),
            eir::Operand::reg(constant(lowBit, eir::Type::bits(32, false, false), nullptr).reg)));
        return result;
    };
    return StringBounds{.left = part(128), .right = part(64), .length = part(0)};
}

std::optional<BodyTranslator::Value> BodyTranslator::runTimeStringAttribute(tree_t attribute,
                                                                            const Value& string) {
    type_t type = tree_type(attribute);
    const auto kind = static_cast<attr_kind_t>(tree_subkind(attribute));
    const bool ascending = string.layout.index.ascending;
    if (kind == ATTR_ASCENDING)
        return booleanOf(constant(ascending ? 1 : 0, eir::Type::bits(1, false, false), nullptr).reg,
                         type);
    const std::optional<eir::Type> storage = scalarStorage(type);
    if (!storage
        || (kind != ATTR_LENGTH && kind != ATTR_LEFT && kind != ATTR_RIGHT && kind != ATTR_LOW
            && kind != ATTR_HIGH)) {
        fail(attribute, std::string("the attribute '") + istr(tree_ident(attribute))
                            + " of a string of run-time length");
        return std::nullopt;
    }
    const StringBounds bounds = stringBounds(string, attribute);
    const Value* chosen = &bounds.length;
    if (kind == ATTR_LEFT || (kind == ATTR_LOW && ascending) || (kind == ATTR_HIGH && !ascending))
        chosen = &bounds.left;
    if (kind == ATTR_RIGHT || (kind == ATTR_HIGH && ascending) || (kind == ATTR_LOW && !ascending))
        chosen = &bounds.right;
    Value result = resize(*chosen, storage->bitWidth());
    result.vhdlType = type;
    return result;
}

std::optional<BodyTranslator::Value>
BodyTranslator::runTimeStringPart(tree_t expression, const Value& string, type_t type) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringSlice;
    call.location = owner.locate(expression);
    call.arguments.push_back(argumentOf(string.reg, Handle));

    if (tree_kind(expression) == T_ARRAY_REF) {
        const std::optional<eir::Type> storage = scalarStorage(type);
        if (tree_params(expression) != 1 || !storage) {
            fail(expression, "this element of a string of run-time length");
            return std::nullopt;
        }
        const std::optional<Value> index = this->expression(tree_value(tree_param(expression, 0)));
        if (!index) return std::nullopt;
        call.arguments.push_back(argumentOf(resize(*index, 64).reg, Index));
        Value element = resize(systemFunction(std::move(call), Character), storage->bitWidth());
        element.vhdlType = type;
        return element;
    }

    const std::optional<Folder::Range> bounds = folder().range(tree_range(expression, 0));
    if (!bounds) {
        fail(expression, "a slice of a string of run-time length whose bounds are not static");
        return std::nullopt;
    }
    if (bounds->length() == 0) {
        const std::optional<Layout> null = nullLayout(*bounds, type_elem(type), folder());
        if (!null) {
            fail(expression, "this null slice of a string of run-time length");
            return std::nullopt;
        }
        return constantArray(eir::BitVector::filled(1, eir::Logic::Zero), *null, type);
    }
    if (bounds->ascending != string.layout.index.ascending) {
        fail(expression, "a slice in the other direction than its string");
        return std::nullopt;
    }
    const std::optional<Layout> layout = arrayLayout(*bounds, type_elem(type), folder());
    if (!layout) {
        fail(expression, "this slice of a string of run-time length");
        return std::nullopt;
    }
    call.arguments.push_back(argumentOf(constant(bounds->left, Index, nullptr).reg, Index));
    Value slice = systemFunction(std::move(call), layout->storage);
    slice.vhdlType = type;
    slice.layout = *layout;
    return slice;
}

bool BodyTranslator::runTimeStringAssignment(tree_t name, const Value& string, tree_t value) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringSplice;
    call.location = owner.locate(name);
    call.arguments.push_back(argumentOf(string.reg, Handle));

    std::optional<Value> written;
    if (tree_kind(name) == T_ARRAY_REF) {
        if (tree_params(name) != 1)
            return fail(name, "this element of a string of run-time length");
        const std::optional<Value> index = expression(tree_value(tree_param(name, 0)));
        if (!index) return false;
        call.arguments.push_back(argumentOf(resize(*index, 64).reg, Index));
        written = expression(value);
        if (!written) return false;
        written = resize(*written, 8);
    } else {
        const std::optional<Folder::Range> bounds = folder().range(tree_range(name, 0));
        if (!bounds)
            return fail(name, "a slice of a string of run-time length whose bounds are not static");
        // A null slice is assigned nothing.
        if (bounds->length() == 0) return true;
        if (bounds->ascending != string.layout.index.ascending)
            return fail(name, "a slice in the other direction than its string");
        const std::optional<Layout> layout =
            arrayLayout(*bounds, type_elem(tree_type(name)), folder());
        if (!layout) return fail(name, "this slice of a string of run-time length");
        written = expression(value, &*layout);
        if (!written) return false;
        if (written->type.bitWidth() != layout->storage.bitWidth()) {
            failLength(name, written->type.bitWidth(), layout->storage.bitWidth(), &*layout,
                       "the slice");
            return true;
        }
        call.arguments.push_back(argumentOf(constant(bounds->left, Index, nullptr).reg, Index));
    }
    call.arguments.push_back(argumentOf(written->reg, written->type));
    emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                eir::Operand::sysCall(addSysCall(std::move(call)))));
    return true;
}

std::optional<BodyTranslator::Value> BodyTranslator::inScratch(tree_t expression) {
    if (const Value* string = runTimeString(expression)) return *string;
    if (isRunTimeSlice(expression)) return sliceToScratch(expression);
    const std::optional<Value> value = this->expression(expression);
    if (!value) return std::nullopt;
    const Layout& layout = value->layout;
    if (!layout.isArray() || !layout.inner.empty() || layout.elementBits != 8) {
        fail(expression, "a string whose bounds are not known here");
        return std::nullopt;
    }
    return copyToScratch(*value, expression);
}

BodyTranslator::Value BodyTranslator::copyToScratch(const Value& value, tree_t where) {
    const Layout& layout = value.layout;
    const auto bound = [&](std::int64_t number) { return constant(number, Index, nullptr); };
    const Value up =
        constant(layout.index.ascending ? 1 : 0, eir::Type::bits(1, false, false), nullptr);
    eir::SysTaskCall call;
    call.location = owner.locate(where);
    // A null one has no elements to take, only its bounds.
    if (layout.isNull()) {
        call.kind = eir::SysTaskKind::StringNew;
        call.arguments = {argumentOf(bound(layout.index.left).reg, Index),
                          argumentOf(bound(layout.index.right).reg, Index),
                          argumentOf(up.reg, up.type),
                          argumentOf(constant(0, Character, nullptr).reg, Character)};
        return systemFunction(std::move(call), Handle);
    }
    call.kind = eir::SysTaskKind::StringFromArray;
    const Value left = bound(layout.index.left);
    const Value right = bound(layout.index.right);
    call.arguments = {argumentOf(value.reg, value.type), argumentOf(left.reg, Index),
                      argumentOf(right.reg, Index),      argumentOf(left.reg, Index),
                      argumentOf(right.reg, Index),      argumentOf(up.reg, up.type)};
    return systemFunction(std::move(call), Handle);
}

void BodyTranslator::releaseMessageScratch() {
    if (!messageScratch) return;
    releaseScratch(*messageScratch);
    messageScratch.reset();
}

bool BodyTranslator::isRunTimeSlice(tree_t expression) const {
    if (tree_kind(expression) != T_ARRAY_SLICE || !isStringType(tree_type(expression)))
        return false;
    tree_t range = tree_range(expression, 0);
    if (folder().range(range)) return false;
    // One whose bounds move together, `d(i to i + 3)`, has a length known here.
    const auto kind = static_cast<range_kind_t>(tree_subkind(range));
    if (kind != RANGE_TO && kind != RANGE_DOWNTO) return true;
    return kind == RANGE_TO ? !folder().difference(tree_right(range), tree_left(range))
                            : !folder().difference(tree_left(range), tree_right(range));
}

bool BodyTranslator::passesInScratch(tree_t actual, type_t formalType) const {
    return isStringType(formalType) && !layoutFor(formalType)
           && (runTimeString(actual) != nullptr || isRunTimeSlice(actual));
}

std::optional<BodyTranslator::Value> BodyTranslator::sliceToScratch(tree_t expression) {
    tree_t range = tree_range(expression, 0);
    const std::optional<Value> array = this->expression(tree_value(expression));
    if (!array) return std::nullopt;
    const Layout& layout = array->layout;
    if (!layout.isArray() || !layout.inner.empty() || layout.elementBits != 8) {
        fail(expression, "a slice of run-time length of a string whose bounds are not known here");
        return std::nullopt;
    }
    const auto kind = static_cast<range_kind_t>(tree_subkind(range));
    if ((kind != RANGE_TO && kind != RANGE_DOWNTO)
        || (kind == RANGE_TO) != layout.index.ascending) {
        fail(range, "a slice whose bounds are not static and run the other way than its array's");
        return std::nullopt;
    }
    const std::optional<Value> left = this->expression(tree_left(range));
    const std::optional<Value> right = this->expression(tree_right(range));
    if (!left || !right) return std::nullopt;

    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringFromArray;
    call.location = owner.locate(expression);
    const Value up =
        constant(layout.index.ascending ? 1 : 0, eir::Type::bits(1, false, false), nullptr);
    call.arguments = {argumentOf(array->reg, array->type),
                      argumentOf(constant(layout.index.left, Index, nullptr).reg, Index),
                      argumentOf(constant(layout.index.right, Index, nullptr).reg, Index),
                      argumentOf(resize(*left, 64).reg, Index),
                      argumentOf(resize(*right, 64).reg, Index),
                      argumentOf(up.reg, up.type)};
    const Value made = systemFunction(std::move(call), Handle);
    type_t type = tree_type(expression);
    return Value{.reg = made.reg,
                 .type = Handle,
                 .vhdlType = type,
                 .layout = runTimeStringLayout(type, layout.index.ascending)};
}

bool BodyTranslator::sliceFromScratch(tree_t expression, const Value& string) {
    tree_t prefix = tree_value(expression);
    const std::optional<Place> target = place(prefix);
    if (!target) return false;
    if (!target->isVariable) return fail(expression, "an out parameter whose actual is a signal");
    const Layout& layout = target->layout;
    const Value before = read(*target, tree_type(prefix));
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringIntoArray;
    call.location = owner.locate(expression);
    call.arguments = {argumentOf(before.reg, before.type),
                      argumentOf(constant(layout.index.left, Index, nullptr).reg, Index),
                      argumentOf(constant(layout.index.right, Index, nullptr).reg, Index),
                      argumentOf(string.reg, Handle)};
    assignVariable(*target, systemFunction(std::move(call), layout.storage));
    return true;
}

std::optional<BodyTranslator::Value> BodyTranslator::scratchActual(tree_t actual,
                                                                   std::optional<Value>& mark) {
    if (const Value* string = runTimeString(actual)) return *string;
    if (!mark) mark = scratchMark();
    return sliceToScratch(actual);
}

BodyTranslator::Value BodyTranslator::stringComparison(const Value& left, const Value& right,
                                                       subprogram_kind_t kind, tree_t expression) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::StringCompare;
    call.location = owner.locate(expression);
    call.arguments = {argumentOf(left.reg, Handle), argumentOf(right.reg, Handle)};
    Value order = systemFunction(std::move(call), eir::Type::bits(8, true, false));
    order.type = eir::Type::bits(8, true, false);

    // The order against 0: `a > b` is `0 < order`.
    const Value zero = constant(0, order.type, nullptr);
    const bool swapped = kind == S_ARRAY_GT || kind == S_ARRAY_GE;
    eir::Opcode opcode = eir::Opcode::Equal;
    if (kind == S_ARRAY_NEQ) opcode = eir::Opcode::NotEqual;
    if (kind == S_ARRAY_LT || kind == S_ARRAY_GT) opcode = eir::Opcode::LessThan;
    if (kind == S_ARRAY_LE || kind == S_ARRAY_GE) opcode = eir::Opcode::LessEqual;
    return booleanOf(compare(opcode, swapped ? zero : order, swapped ? order : zero),
                     tree_type(expression));
}

std::optional<BodyTranslator::Value>
BodyTranslator::runTimeStringComparison(tree_t expression, subprogram_kind_t kind) {
    if (tree_params(expression) != 2) {
        fail(expression, "this operator on a string of run-time length");
        return std::nullopt;
    }
    // A string of a length known here is compared as one in scratch, released once compared.
    const Value mark = scratchMark();
    const std::optional<Value> left = inScratch(tree_value(tree_param(expression, 0)));
    if (!left) return std::nullopt;
    const std::optional<Value> right = inScratch(tree_value(tree_param(expression, 1)));
    if (!right) return std::nullopt;
    Value result = stringComparison(*left, *right, kind, expression);
    releaseScratch(mark);
    return result;
}

} // namespace enzan::frontend::vhdl
