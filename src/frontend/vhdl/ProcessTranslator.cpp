// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/ProcessTranslator.h"

#include "frontend/vhdl/Types.h"

#include <algorithm>
#include <string>
#include <utility>

namespace enzan::frontend::vhdl {

namespace {

/// The positions of `std.standard.severity_level`.
constexpr std::int64_t SeverityNote = 0;
constexpr std::int64_t SeverityError = 2;

/// True when `name`, a subprogram's mangled name, is `prefix` or one of its overloads.
bool names(ident_t name, std::string_view prefix) {
    const std::string_view text = istr(name);
    return text.starts_with(prefix)
           && (text.size() == prefix.size() || text[prefix.size()] == '('
               || text[prefix.size()] == '$');
}

/// True when anything in `process` can suspend it: a `wait`, or a call of a procedure, which may
/// contain one.
bool canSuspend(tree_t process) {
    return tree_visit_only(process, nullptr, nullptr, T_WAIT) > 0
           || tree_visit_only(process, nullptr, nullptr, T_PCALL) > 0;
}

} // namespace

ProcessTranslator::ProcessTranslator(ArchitectureScope& scope, tree_t statement, std::string name)
    : owner(scope), source(statement) {
    process.name = std::move(name);
    process.kind = eir::ProcessKind::Always;
    process.drivesSignals = true;
    process.location = owner.locate(statement);
}

// --- emission ---------------------------------------------------------------

std::uint32_t ProcessTranslator::newRegister(eir::Type type) {
    process.registerTypes.push_back(type);
    return static_cast<std::uint32_t>(process.registerTypes.size() - 1);
}

std::uint32_t ProcessTranslator::newBlock(std::string label) {
    eir::BasicBlock block;
    block.label = std::move(label);
    process.blocks.push_back(std::move(block));
    return static_cast<std::uint32_t>(process.blocks.size() - 1);
}

void ProcessTranslator::selectBlock(std::uint32_t block) {
    current = block;
}

bool ProcessTranslator::terminated() const {
    const std::vector<eir::Instruction>& instructions = process.blocks[current].instructions;
    return !instructions.empty() && eir::isTerminator(instructions.back().opcode);
}

void ProcessTranslator::emit(const eir::Instruction& instruction) {
    eir::Instruction stamped = instruction;
    stamped.location = statementLocation;
    process.blocks[current].instructions.push_back(stamped);
}

void ProcessTranslator::jump(std::uint32_t block) {
    if (!terminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(block)));
}

std::uint32_t ProcessTranslator::addConstant(const eir::BitVector& value) {
    process.constants.push_back(value);
    return static_cast<std::uint32_t>(process.constants.size() - 1);
}

std::uint32_t ProcessTranslator::addString(std::string text) {
    process.strings.push_back(std::move(text));
    return static_cast<std::uint32_t>(process.strings.size() - 1);
}

std::uint32_t ProcessTranslator::addSysCall(eir::SysTaskCall call) {
    process.sysCalls.push_back(std::move(call));
    return static_cast<std::uint32_t>(process.sysCalls.size() - 1);
}

ProcessTranslator::Value ProcessTranslator::constant(std::int64_t value, eir::Type type,
                                                     type_t vhdlType) {
    const std::uint32_t reg = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(reg),
                                eir::Operand::constant(addConstant(encode(value, type)))));
    return Value{.reg = reg, .type = type, .vhdlType = vhdlType};
}

ProcessTranslator::Value ProcessTranslator::resize(const Value& value, std::uint32_t width) {
    if (value.type.bitWidth() == width) return value;
    const eir::Type type = eir::Type::bits(width, value.type.isSigned(), false);
    const std::uint32_t reg = newRegister(type);
    const eir::Opcode opcode =
        width > value.type.bitWidth() ? eir::Opcode::Extend : eir::Opcode::Truncate;
    emit(value.type.isSigned() ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(reg),
                                                              eir::Operand::reg(value.reg))
                               : eir::Instruction::make(opcode, eir::Operand::reg(reg),
                                                        eir::Operand::reg(value.reg)));
    return Value{.reg = reg, .type = type, .vhdlType = value.vhdlType};
}

void ProcessTranslator::noteRead(std::uint32_t objectIndex) {
    reads.insert(objectIndex);
}

void ProcessTranslator::noteWrite(std::uint32_t objectIndex) {
    written.insert(objectIndex);
}

bool ProcessTranslator::fail(tree_t where, std::string_view what) {
    owner.unsupported(where, what);
    return false;
}

// --- the process --------------------------------------------------------------

std::optional<eir::Process> ProcessTranslator::translate() {
    if (!canSuspend(source)) {
        owner.reportNeverWaits(source);
        return std::nullopt;
    }

    const std::uint32_t entry = newBlock("entry");
    selectBlock(entry);
    statementLocation = owner.locate(source);

    const unsigned declarations = tree_decls(source);
    for (unsigned index = 0; index < declarations; ++index) {
        if (!declare(tree_decl(source, index))) return std::nullopt;
    }

    const std::uint32_t loop = newBlock("process");
    jump(loop);
    selectBlock(loop);
    if (!statements(source)) return std::nullopt;
    jump(loop);

    process.readSet.assign(reads.begin(), reads.end());
    for (const std::uint32_t objectIndex : written) {
        process.writeSet.push_back(eir::WriteExtent::whole(
            objectIndex, owner.definition().objects[objectIndex].type.bitWidth()));
    }
    eir::normaliseWriteSet(process.writeSet);
    return std::move(process);
}

bool ProcessTranslator::declare(tree_t declaration) {
    switch (tree_kind(declaration)) {
    case T_VAR_DECL:
    case T_CONST_DECL: {
        statementLocation = owner.locate(declaration);
        type_t type = tree_type(declaration);
        const std::optional<eir::Type> storage = storageOf(type);
        if (!storage) return fail(declaration, std::string("a variable of type ") + type_pp(type));

        // A variable starts at its initial value, or at its type's left bound (IEEE 1076 section
        // 14.4.2.5); a constant always has a value, static or not.
        std::optional<Value> initial;
        if (tree_has_value(declaration)) {
            initial = expression(tree_value(declaration));
            if (!initial) return false;
        } else {
            const std::optional<std::int64_t> left = owner.folder().left(type);
            if (!left)
                return fail(declaration, std::string("the initial value of a ") + type_pp(type));
            initial = constant(*left, *storage, type);
        }

        const Value variable{.reg = newRegister(*storage), .type = *storage, .vhdlType = type};
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(variable.reg),
                                    eir::Operand::reg(resize(*initial, storage->bitWidth()).reg)));
        locals[declaration] = variable;
        return true;
    }

    // Declarations that make nothing at run time.
    case T_TYPE_DECL:
    case T_SUBTYPE_DECL:
    case T_ATTR_DECL:
    case T_ATTR_SPEC:
    case T_USE:
    case T_FUNC_DECL:
    case T_PROC_DECL: return true;

    default:
        return fail(declaration,
                    std::string("a ") + tree_kind_str(tree_kind(declaration)) + " in a process");
    }
}

bool ProcessTranslator::statements(tree_t container) {
    const unsigned count = tree_stmts(container);
    for (unsigned index = 0; index < count; ++index) {
        // What follows an `exit` or a `next` cannot run, but it is still translated, into a block
        // nothing reaches, so that it is checked as everything else is.
        if (terminated()) selectBlock(newBlock("unreachable"));
        if (!statement(tree_stmt(container, index))) return false;
    }
    return true;
}

bool ProcessTranslator::statement(tree_t statement) {
    statementLocation = owner.locate(statement);
    switch (tree_kind(statement)) {
    case T_SIGNAL_ASSIGN: return signalAssignment(statement);
    case T_VAR_ASSIGN: return variableAssignment(statement);
    case T_IF: return ifStatement(statement);
    case T_CASE: return caseStatement(statement);
    case T_WHILE:
    case T_LOOP: return whileLoop(statement);
    case T_FOR: return forLoop(statement);
    case T_EXIT: return loopJump(statement, true);
    case T_NEXT: return loopJump(statement, false);
    case T_WAIT: return waitStatement(statement);
    case T_ASSERT: return assertion(statement);
    case T_REPORT: return report(statement);
    case T_PCALL: return procedureCall(statement);
    case T_NULL: return true;
    default:
        return fail(statement, std::string("the statement ") + tree_kind_str(tree_kind(statement)));
    }
}

std::optional<std::uint32_t> ProcessTranslator::targetObject(tree_t target) {
    if (tree_kind(target) != T_REF || !tree_has_ref(target)) {
        fail(target, "assigning part of a signal, or an aggregate of signals");
        return std::nullopt;
    }
    const std::optional<std::uint32_t> object = owner.objectFor(tree_ref(target));
    if (!object) fail(target, "assigning a signal that is not declared by the architecture");
    return object;
}

bool ProcessTranslator::signalAssignment(tree_t statement) {
    const std::optional<std::uint32_t> object = targetObject(tree_target(statement));
    if (!object) return false;
    const std::uint32_t width = owner.definition().objects[*object].type.bitWidth();
    const unsigned count = tree_waveforms(statement);
    if (count == 0) return fail(statement, "an assignment with no waveform");

    // NVC records the delay mechanism as the rejection limit: none, or the first element's own
    // delay, for the default inertial delay; zero for `transport`; anything else for an explicit
    // `reject ... inertial`, which needs a limit other than the delay and is not handled yet.
    bool inertial = true;
    if (tree_has_reject(statement)) {
        tree_t reject = tree_reject(statement);
        tree_t first = tree_waveform(statement, 0);
        if (!tree_has_delay(first) || reject != tree_delay(first)) {
            const std::optional<std::int64_t> limit = owner.folder().integer(reject);
            const std::optional<std::int64_t> firstDelay =
                tree_has_delay(first) ? owner.folder().integer(tree_delay(first))
                                      : std::optional<std::int64_t>(0);
            if (limit == 0) {
                inertial = false;
            } else if (!limit || limit != firstDelay) {
                return fail(reject, "a rejection limit other than the delay");
            }
        }
    }

    // Each element of the waveform is a transaction of this process's driver of the signal. The
    // first replaces what the driver had pending from its time on; the rest are appended after
    // it, which is a transport assignment of each (IEEE 1076 section 10.5.2.2).
    const eir::Type timeType = eir::Type::bits(64, true, false);
    for (unsigned index = 0; index < count; ++index) {
        tree_t waveform = tree_waveform(statement, index);
        if (!tree_has_value(waveform)) return fail(waveform, "a null transaction");

        const std::optional<Value> value = expression(tree_value(waveform));
        if (!value) return false;
        const Value stored = resize(*value, width);

        const std::optional<Value> after = tree_has_delay(waveform)
                                               ? delay(tree_delay(waveform))
                                               : std::optional(constant(0, timeType, nullptr));
        if (!after) return false;

        const bool rejects = inertial && index == 0;
        emit(eir::Instruction::make(rejects ? eir::Opcode::Drive : eir::Opcode::DriveTransport, {},
                                    eir::Operand::object(*object), eir::Operand::reg(stored.reg),
                                    eir::Operand::reg(after->reg)));
    }
    noteWrite(*object);
    return true;
}

bool ProcessTranslator::variableAssignment(tree_t statement) {
    tree_t target = tree_target(statement);
    if (tree_kind(target) != T_REF || !tree_has_ref(target))
        return fail(target, "assigning part of a variable, or an aggregate of variables");
    const auto variable = locals.find(tree_ref(target));
    if (variable == locals.end())
        return fail(target, "assigning a variable that is not declared by the process");

    const std::optional<Value> value = expression(tree_value(statement));
    if (!value) return false;
    emit(eir::Instruction::make(
        eir::Opcode::Move, eir::Operand::reg(variable->second.reg),
        eir::Operand::reg(resize(*value, variable->second.type.bitWidth()).reg)));
    return true;
}

bool ProcessTranslator::ifStatement(tree_t statement) {
    const std::uint32_t join = newBlock("if-join");
    const unsigned count = tree_conds(statement);
    for (unsigned index = 0; index < count; ++index) {
        tree_t branch = tree_cond(statement, index);
        if (!tree_has_value(branch)) {
            if (!statements(branch)) return false;
            jump(join);
            break;
        }

        statementLocation = owner.locate(branch);
        const std::optional<Value> condition = expression(tree_value(branch));
        if (!condition) return false;
        const std::uint32_t taken = newBlock("if-then");
        const std::uint32_t next = newBlock("if-else");
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                    eir::Operand::block(taken), eir::Operand::block(next)));
        selectBlock(taken);
        if (!statements(branch)) return false;
        jump(join);
        selectBlock(next);
    }
    jump(join);
    selectBlock(join);
    return true;
}

std::optional<ProcessTranslator::Value> ProcessTranslator::choiceTest(const Value& selector,
                                                                      tree_t choice) {
    if (tree_has_name(choice)) {
        const std::optional<Value> value = expression(tree_name(choice));
        if (!value) return std::nullopt;
        return Value{
            .reg = compare(eir::Opcode::Equal, selector, resize(*value, selector.type.bitWidth())),
            .type = eir::Type::bits(1, false, false),
            .vhdlType = nullptr};
    }

    if (tree_ranges(choice) == 1) {
        const std::optional<Folder::Range> range = owner.folder().range(tree_range(choice, 0));
        if (!range) {
            fail(choice, "a choice whose range is not static");
            return std::nullopt;
        }
        const Value low = constant(range->low(), selector.type, selector.vhdlType);
        const Value high = constant(range->high(), selector.type, selector.vhdlType);
        const std::uint32_t above = compare(eir::Opcode::LessEqual, low, selector);
        const std::uint32_t below = compare(eir::Opcode::LessEqual, selector, high);
        const std::uint32_t both = newRegister(eir::Type::bits(1, false, false));
        emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(both),
                                    eir::Operand::reg(above), eir::Operand::reg(below)));
        return Value{.reg = both, .type = eir::Type::bits(1, false, false), .vhdlType = nullptr};
    }

    fail(choice, "this kind of choice");
    return std::nullopt;
}

bool ProcessTranslator::caseStatement(tree_t statement) {
    const std::optional<Value> selector = expression(tree_value(statement));
    if (!selector) return false;
    if (!selector->type.isBits() || !type_is_scalar(selector->vhdlType))
        return fail(statement, "a case statement whose selector is not of a discrete type");

    // A chain of tests in the order the alternatives are written. The choices of a case statement
    // cover every value once and only once (IEEE 1076 section 10.9), so the order changes nothing
    // but how many tests a value passes on the way.
    const std::uint32_t join = newBlock("case-join");
    const unsigned alternatives = tree_stmts(statement);
    for (unsigned index = 0; index < alternatives; ++index) {
        tree_t alternative = tree_stmt(statement, index);
        const std::uint32_t body = newBlock("case-item");
        const unsigned choices = tree_choices(alternative);
        for (unsigned choiceIndex = 0; choiceIndex < choices; ++choiceIndex) {
            tree_t choice = tree_choice(alternative, choiceIndex);
            const bool others = !tree_has_name(choice) && tree_ranges(choice) == 0;
            if (others) {
                jump(body);
                break;
            }
            const std::optional<Value> test = choiceTest(*selector, choice);
            if (!test) return false;
            const std::uint32_t next = newBlock("case-test");
            emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(test->reg),
                                        eir::Operand::block(body), eir::Operand::block(next)));
            selectBlock(next);
        }
        const std::uint32_t untested = current;

        selectBlock(body);
        if (!statements(alternative)) return false;
        jump(join);
        selectBlock(untested);
    }
    jump(join);
    selectBlock(join);
    return true;
}

bool ProcessTranslator::whileLoop(tree_t statement) {
    const std::uint32_t head = newBlock("loop-head");
    const std::uint32_t body = newBlock("loop-body");
    const std::uint32_t exit = newBlock("loop-exit");
    jump(head);

    selectBlock(head);
    if (tree_kind(statement) == T_WHILE && tree_has_value(statement)) {
        const std::optional<Value> condition = expression(tree_value(statement));
        if (!condition) return false;
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                    eir::Operand::block(body), eir::Operand::block(exit)));
    } else {
        jump(body);
    }

    selectBlock(body);
    loops.push_back(Loop{.label = tree_has_ident(statement) ? tree_ident(statement) : nullptr,
                         .exitBlock = exit,
                         .nextBlock = head});
    const bool translated = statements(statement);
    loops.pop_back();
    if (!translated) return false;
    jump(head);

    selectBlock(exit);
    return true;
}

bool ProcessTranslator::forLoop(tree_t statement) {
    if (tree_decls(statement) != 1 || tree_ranges(statement) != 1)
        return fail(statement, "this form of for loop");
    tree_t parameter = tree_decl(statement, 0);
    type_t type = tree_type(parameter);
    const std::optional<eir::Type> storage = storageOf(type);
    if (!storage) return fail(statement, std::string("a loop over ") + type_pp(type));

    // The bounds are evaluated once, before the first iteration (IEEE 1076 section 10.10).
    tree_t range = tree_range(statement, 0);
    const bool ascending = tree_subkind(range) == RANGE_TO;
    if (tree_subkind(range) != RANGE_TO && tree_subkind(range) != RANGE_DOWNTO) {
        const std::optional<Folder::Range> folded = owner.folder().range(range);
        if (!folded) return fail(range, "a loop over a range that is not static");
        return fail(range, "a loop over an attribute's range");
    }
    const std::optional<Value> left = expression(tree_left(range));
    const std::optional<Value> right = expression(tree_right(range));
    if (!left || !right) return false;
    const Value first = resize(*left, storage->bitWidth());
    const Value last = resize(*right, storage->bitWidth());

    const Value index{.reg = newRegister(*storage), .type = *storage, .vhdlType = type};
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(index.reg),
                                eir::Operand::reg(first.reg)));

    // A null range runs no iteration at all. Otherwise the loop runs from the left bound and stops
    // after the right one, testing for the last value before stepping rather than after, so that a
    // range ending at the type's last value does not step past it.
    const std::uint32_t body = newBlock("for-body");
    const std::uint32_t step = newBlock("for-step");
    const std::uint32_t advance = newBlock("for-advance");
    const std::uint32_t exit = newBlock("for-exit");
    const std::uint32_t empty = ascending ? compare(eir::Opcode::LessThan, last, first)
                                          : compare(eir::Opcode::LessThan, first, last);
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(empty),
                                eir::Operand::block(exit), eir::Operand::block(body)));

    selectBlock(body);
    locals[parameter] = index;
    loops.push_back(Loop{.label = tree_has_ident(statement) ? tree_ident(statement) : nullptr,
                         .exitBlock = exit,
                         .nextBlock = step});
    const bool translated = statements(statement);
    loops.pop_back();
    if (!translated) return false;
    jump(step);

    selectBlock(step);
    const std::uint32_t done = compare(eir::Opcode::Equal, index, last);
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(done),
                                eir::Operand::block(exit), eir::Operand::block(advance)));

    selectBlock(advance);
    const Value one = constant(1, *storage, type);
    const std::uint32_t stepped = newRegister(*storage);
    emit(eir::Instruction::make(ascending ? eir::Opcode::Add : eir::Opcode::Subtract,
                                eir::Operand::reg(stepped), eir::Operand::reg(index.reg),
                                eir::Operand::reg(one.reg)));
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(index.reg),
                                eir::Operand::reg(stepped)));
    jump(body);

    selectBlock(exit);
    return true;
}

bool ProcessTranslator::loopJump(tree_t statement, bool isExit) {
    // An unlabelled `exit` or `next` leaves the innermost loop; a labelled one the loop of that
    // label, which may be further out.
    auto target = loops.rbegin();
    if (tree_has_ident2(statement)) {
        ident_t label = tree_ident2(statement);
        target = std::ranges::find_if(loops.rbegin(), loops.rend(),
                                      [label](const Loop& loop) { return loop.label == label; });
    }
    if (target == loops.rend()) return fail(statement, "an exit or next outside its loop");
    const std::uint32_t destination = isExit ? target->exitBlock : target->nextBlock;

    if (!tree_has_value(statement)) {
        jump(destination);
        return true;
    }
    const std::optional<Value> condition = expression(tree_value(statement));
    if (!condition) return false;
    const std::uint32_t stay = newBlock(isExit ? "exit-not-taken" : "next-not-taken");
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                eir::Operand::block(destination), eir::Operand::block(stay)));
    selectBlock(stay);
    return true;
}

bool ProcessTranslator::waitStatement(tree_t statement) {
    const bool hasDelay = tree_has_delay(statement);
    const bool hasCondition = tree_has_value(statement);
    const unsigned triggerCount = tree_triggers(statement);

    if (hasDelay && (triggerCount > 0 || hasCondition))
        return fail(statement, "a wait with both a timeout and a sensitivity or a condition");

    if (hasDelay) {
        const std::optional<Value> amount = delay(tree_delay(statement));
        if (!amount) return false;
        const std::uint32_t resume = newBlock("after-wait");
        emit(eir::Instruction::make(eir::Opcode::WaitTime, {}, eir::Operand::reg(amount->reg),
                                    eir::Operand::block(resume)));
        selectBlock(resume);
        return true;
    }

    // `wait;`, `wait on`, and `wait until`, which is a `wait on` what its condition reads repeated
    // until the condition holds (IEEE 1076 section 10.2). NVC fills in the sensitivity of a `wait
    // until` that names none.
    std::vector<eir::SensitivityEntry> entries;
    for (unsigned index = 0; index < triggerCount; ++index) {
        tree_t trigger = tree_trigger(statement, index);
        if (tree_kind(trigger) != T_REF || !tree_has_ref(trigger))
            return fail(trigger, "waiting on part of a signal");
        const std::optional<std::uint32_t> object = owner.objectFor(tree_ref(trigger));
        if (!object)
            return fail(trigger, "waiting on something that is not a signal of the architecture");
        noteRead(*object);
        entries.push_back(eir::SensitivityEntry{.objectIndex = *object});
    }
    if (hasCondition && entries.empty())
        return fail(statement, "a wait until whose condition reads no signal");

    const auto list = static_cast<std::uint32_t>(process.eventLists.size());
    process.eventLists.push_back(std::move(entries));
    const std::uint32_t suspend = newBlock("wait");
    const std::uint32_t test = hasCondition ? newBlock("wait-test") : 0;
    const std::uint32_t resume = newBlock("after-wait");
    jump(suspend);
    selectBlock(suspend);
    emit(eir::Instruction::make(eir::Opcode::WaitEvents, {}, eir::Operand::immediate(list),
                                eir::Operand::block(hasCondition ? test : resume)));
    if (hasCondition) {
        selectBlock(test);
        const std::optional<Value> condition = expression(tree_value(statement));
        if (!condition) return false;
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                    eir::Operand::block(resume), eir::Operand::block(suspend)));
    }
    selectBlock(resume);
    return true;
}

bool ProcessTranslator::assertion(tree_t statement) {
    const std::optional<Value> condition = expression(tree_value(statement));
    if (!condition) return false;

    const std::uint32_t failed = newBlock("assert-fail");
    const std::uint32_t join = newBlock("assert-join");
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                eir::Operand::block(join), eir::Operand::block(failed)));

    selectBlock(failed);
    std::optional<Value> severity;
    if (tree_has_severity(statement)) {
        severity = expression(tree_severity(statement));
        if (!severity) return false;
    }
    // An assertion's default severity is error, and its default message the standard's own
    // (IEEE 1076 section 10.3).
    if (!severity) severity = constant(SeverityError, eir::Type::bits(8, false, false), nullptr);
    if (!emitReport(statement, *severity,
                    tree_has_message(statement) ? tree_message(statement) : nullptr,
                    "Assertion violation."))
        return false;
    jump(join);
    selectBlock(join);
    return true;
}

bool ProcessTranslator::report(tree_t statement) {
    std::optional<Value> severity;
    if (tree_has_severity(statement)) {
        severity = expression(tree_severity(statement));
        if (!severity) return false;
    } else {
        severity = constant(SeverityNote, eir::Type::bits(8, false, false), nullptr);
    }
    return emitReport(statement, *severity, tree_message(statement), "");
}

bool ProcessTranslator::emitReport(tree_t where, const Value& severity, tree_t message,
                                   std::string_view fallback) {
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::Report;
    call.location = owner.locate(where);
    call.arguments.push_back(
        eir::SysTaskArgument{.value = eir::Operand::reg(severity.reg), .type = severity.type});

    std::string format;
    if (message != nullptr) {
        if (!messageParts(message, format, call.arguments)) return false;
    } else {
        format = fallback;
    }
    call.formatIndex = addString(std::move(format));
    emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                eir::Operand::sysCall(addSysCall(std::move(call)))));
    return true;
}

bool ProcessTranslator::messageParts(tree_t message, std::string& format,
                                     std::vector<eir::SysTaskArgument>& arguments) {
    switch (tree_kind(message)) {
    case T_STRING: {
        const unsigned count = tree_chars(message);
        for (unsigned index = 0; index < count; ++index) {
            // Each character is a reference to a literal of `character`, named with its quotes.
            const std::string_view literal = istr(tree_ident(tree_char(message, index)));
            const char character = literal.size() == 3 ? literal[1] : '?';
            if (character == '%') format += '%';
            format += character;
        }
        return true;
    }

    case T_REF:
        // A single character, as in `"a" & lf`, or a constant string.
        if (tree_has_ref(message) && tree_kind(tree_ref(message)) == T_ENUM_LIT) {
            const std::string_view literal = istr(tree_ident(tree_ref(message)));
            if (literal.size() == 3) {
                if (literal[1] == '%') format += '%';
                format += literal[1];
                return true;
            }
        }
        break;

    // NVC's simplification turns a chain of `&` into one aggregate of the pieces: whole arrays to
    // concatenate, and single characters by position.
    case T_AGGREGATE: {
        const unsigned count = tree_assocs(message);
        for (unsigned index = 0; index < count; ++index) {
            tree_t piece = tree_assoc(message, index);
            if (tree_subkind(piece) != A_CONCAT && tree_subkind(piece) != A_POS) {
                return fail(piece, "a message aggregate with named or ranged elements");
            }
            if (!messageParts(tree_value(piece), format, arguments)) return false;
        }
        return true;
    }

    case T_FCALL: {
        if (!tree_has_ref(message)) break;
        const auto kind = static_cast<subprogram_kind_t>(tree_subkind(tree_ref(message)));
        if (kind == S_CONCAT && tree_params(message) == 2) {
            return messageParts(tree_value(tree_param(message, 0)), format, arguments)
                   && messageParts(tree_value(tree_param(message, 1)), format, arguments);
        }
        if (kind == S_TO_STRING && tree_params(message) == 1) {
            tree_t operand = tree_value(tree_param(message, 0));
            if (!isSignedScalar(tree_type(operand))) break;
            const std::optional<Value> value = expression(operand);
            if (!value) return false;
            format += type_is_physical(tree_type(operand)) ? "%0d fs" : "%0d";
            arguments.push_back(
                eir::SysTaskArgument{.value = eir::Operand::reg(value->reg), .type = value->type});
            return true;
        }
        break;
    }

    case T_ATTR_REF: {
        if (tree_subkind(message) != ATTR_IMAGE || tree_params(message) != 1) break;
        tree_t operand = tree_value(tree_param(message, 0));
        if (!isSignedScalar(tree_type(operand))) break;
        const std::optional<Value> value = expression(operand);
        if (!value) return false;
        // A physical value's image is in its primary unit, which for `time` is fs.
        format += type_is_physical(tree_type(operand)) ? "%0d fs" : "%0d";
        arguments.push_back(
            eir::SysTaskArgument{.value = eir::Operand::reg(value->reg), .type = value->type});
        return true;
    }

    default: break;
    }
    return fail(message, "a message built other than from string literals, characters and the "
                         "'image of integers and times");
}

bool ProcessTranslator::procedureCall(tree_t statement) {
    if (!tree_has_ref(statement)) return fail(statement, "this procedure call");
    tree_t declaration = tree_ref(statement);
    ident_t name = tree_ident2(declaration);

    // `std.env.stop` and `std.env.finish` end the simulation; a status is not acted on yet.
    if (names(name, "STD.ENV.FINISH") || names(name, "STD.ENV.STOP")) {
        eir::SysTaskCall call;
        call.kind = eir::SysTaskKind::Finish;
        call.location = owner.locate(statement);
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(call)))));
        return true;
    }
    return fail(statement, std::string("a call of ") + istr(name));
}

std::optional<ProcessTranslator::Value> ProcessTranslator::delay(tree_t delay) {
    const std::int64_t scale = femtosecondsPerUnit();
    const eir::Type type = eir::Type::bits(64, true, false);

    if (const std::optional<std::int64_t> folded = owner.folder().integer(delay)) {
        if (*folded < 0) {
            fail(delay, "a negative delay");
            return std::nullopt;
        }
        return constant(*folded / scale, type, tree_type(delay));
    }

    const std::optional<Value> value = expression(delay);
    if (!value || scale == 1) return value;
    const Value divisor = constant(scale, type, nullptr);
    const std::uint32_t reg = newRegister(type);
    emit(eir::Instruction::makeSigned(eir::Opcode::Divide, eir::Operand::reg(reg),
                                      eir::Operand::reg(value->reg),
                                      eir::Operand::reg(divisor.reg)));
    return Value{.reg = reg, .type = type, .vhdlType = value->vhdlType};
}

std::int64_t ProcessTranslator::femtosecondsPerUnit() const {
    std::int64_t scale = 1;
    for (int exponent = -15; exponent < owner.timeUnitExponent(); ++exponent) scale *= 10;
    return scale;
}

// --- expressions ---------------------------------------------------------------

std::optional<ProcessTranslator::Value> ProcessTranslator::expression(tree_t expression) {
    type_t type = tree_has_type(expression) ? tree_type(expression) : nullptr;

    // Anything static is folded first, so that a generic, a constant or an attribute of a type
    // costs nothing at run time.
    if (type != nullptr && type_is_scalar(type) && !type_is_real(type)) {
        if (const std::optional<std::int64_t> folded = owner.folder().integer(expression)) {
            const std::optional<eir::Type> storage = storageOf(type);
            if (storage) return constant(*folded, *storage, type);
        }
    }

    switch (tree_kind(expression)) {
    case T_REF: return reference(expression);
    case T_FCALL: return call(expression);
    case T_QUALIFIED: return this->expression(tree_value(expression));
    case T_TYPE_CONV: return conversion(expression);
    case T_ATTR_REF: return attribute(expression);
    case T_LITERAL: fail(expression, "a real literal"); return std::nullopt;
    default:
        fail(expression, std::string("the expression ") + tree_kind_str(tree_kind(expression)));
        return std::nullopt;
    }
}

std::optional<ProcessTranslator::Value> ProcessTranslator::reference(tree_t expression) {
    if (!tree_has_ref(expression)) {
        fail(expression, "a name NVC left unresolved");
        return std::nullopt;
    }
    tree_t declaration = tree_ref(expression);
    type_t type = tree_type(expression);

    if (const auto local = locals.find(declaration); local != locals.end()) return local->second;

    switch (tree_kind(declaration)) {
    case T_SIGNAL_DECL:
    case T_PORT_DECL: {
        const std::optional<std::uint32_t> object = owner.objectFor(declaration);
        if (!object) break;
        const eir::Type storage = owner.definition().objects[*object].type;
        const std::uint32_t reg = newRegister(storage);
        emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(reg),
                                    eir::Operand::object(*object)));
        noteRead(*object);
        return Value{.reg = reg, .type = storage, .vhdlType = type};
    }

    case T_ALIAS: return this->expression(tree_value(declaration));

    default: break;
    }
    fail(expression, std::string("a reference to a ") + tree_kind_str(tree_kind(declaration)));
    return std::nullopt;
}

std::optional<ProcessTranslator::Value> ProcessTranslator::conversion(tree_t expression) {
    type_t target = tree_type(expression);
    tree_t operand = tree_value(expression);
    const std::optional<eir::Type> storage = storageOf(target);
    if (!storage || !type_is_scalar(target) || !isSignedScalar(target)
        || !isSignedScalar(tree_type(operand))) {
        fail(expression, std::string("a conversion to ") + type_pp(target));
        return std::nullopt;
    }
    const std::optional<Value> value = this->expression(operand);
    if (!value) return std::nullopt;
    Value converted = resize(*value, storage->bitWidth());
    converted.vhdlType = target;
    return converted;
}

std::optional<ProcessTranslator::Value> ProcessTranslator::attribute(tree_t expression) {
    const auto kind = static_cast<attr_kind_t>(tree_subkind(expression));
    type_t resultType = tree_type(expression);
    const std::optional<eir::Type> storage = storageOf(resultType);

    switch (kind) {
    // A discrete value is held as its position, so a conversion between a position and a value is
    // one between their storages; a successor or predecessor is one step along.
    case ATTR_POS:
    case ATTR_VAL:
    case ATTR_SUCC:
    case ATTR_PRED: {
        if (!storage || tree_params(expression) != 1) break;
        const std::optional<Value> operand =
            this->expression(tree_value(tree_param(expression, 0)));
        if (!operand) return std::nullopt;
        Value result = resize(*operand, storage->bitWidth());
        result.vhdlType = resultType;
        if (kind == ATTR_SUCC || kind == ATTR_PRED) {
            const Value one = constant(1, result.type, nullptr);
            const std::uint32_t reg = newRegister(result.type);
            emit(eir::Instruction::make(
                kind == ATTR_SUCC ? eir::Opcode::Add : eir::Opcode::Subtract,
                eir::Operand::reg(reg), eir::Operand::reg(result.reg), eir::Operand::reg(one.reg)));
            result.reg = reg;
        }
        return result;
    }

    default: break;
    }
    fail(expression, std::string("the attribute '") + istr(tree_ident(expression)));
    return std::nullopt;
}

std::optional<ProcessTranslator::Value> ProcessTranslator::call(tree_t expression) {
    if (!tree_has_ref(expression)) {
        fail(expression, "a call NVC left unresolved");
        return std::nullopt;
    }
    tree_t declaration = tree_ref(expression);
    const auto kind = static_cast<subprogram_kind_t>(tree_subkind(declaration));
    if (kind != S_USER) return predefined(expression, kind);

    // `now` counts femtoseconds, which EIR's time counts in its own unit.
    if (names(tree_ident2(declaration), "STD.STANDARD.NOW")) {
        const eir::Type type = eir::Type::bits(64, true, false);
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::ReadTime, eir::Operand::reg(reg)));
        Value time{.reg = reg, .type = type, .vhdlType = tree_type(expression)};
        const std::int64_t scale = femtosecondsPerUnit();
        if (scale == 1) return time;
        const Value factor = constant(scale, type, nullptr);
        const std::uint32_t scaled = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::Multiply, eir::Operand::reg(scaled),
                                    eir::Operand::reg(time.reg), eir::Operand::reg(factor.reg)));
        time.reg = scaled;
        return time;
    }

    fail(expression, std::string("a call of ") + istr(tree_ident2(declaration)));
    return std::nullopt;
}

ProcessTranslator::Value ProcessTranslator::booleanOf(std::uint32_t bit, type_t booleanType) {
    const eir::Type type = eir::Type::bits(8, false, false);
    const std::uint32_t reg = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::Extend, eir::Operand::reg(reg),
                                eir::Operand::reg(bit)));
    return Value{.reg = reg, .type = type, .vhdlType = booleanType};
}

std::uint32_t ProcessTranslator::compare(eir::Opcode opcode, const Value& left,
                                         const Value& right) {
    const std::uint32_t reg = newRegister(eir::Type::bits(1, false, false));
    const bool isSigned = left.type.isSigned() && right.type.isSigned();
    emit(isSigned
             ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(reg),
                                            eir::Operand::reg(left.reg),
                                            eir::Operand::reg(right.reg))
             : eir::Instruction::make(opcode, eir::Operand::reg(reg), eir::Operand::reg(left.reg),
                                      eir::Operand::reg(right.reg)));
    return reg;
}

ProcessTranslator::Value ProcessTranslator::modulo(const Value& dividend, const Value& divisor) {
    // `rem`, then the divisor added where the remainder is not zero and its sign differs from the
    // divisor's.
    const eir::Type type = dividend.type;
    const std::uint32_t remainder = newRegister(type);
    emit(eir::Instruction::makeSigned(eir::Opcode::Modulo, eir::Operand::reg(remainder),
                                      eir::Operand::reg(dividend.reg),
                                      eir::Operand::reg(divisor.reg)));
    const Value zero = constant(0, type, nullptr);
    const Value remainderValue{.reg = remainder, .type = type, .vhdlType = dividend.vhdlType};
    const std::uint32_t nonZero = compare(eir::Opcode::NotEqual, remainderValue, zero);
    const std::uint32_t remainderNegative = compare(eir::Opcode::LessThan, remainderValue, zero);
    const std::uint32_t divisorNegative = compare(eir::Opcode::LessThan, divisor, zero);
    const eir::Type bit = eir::Type::bits(1, false, false);
    const std::uint32_t signsDiffer = newRegister(bit);
    emit(eir::Instruction::make(eir::Opcode::Xor, eir::Operand::reg(signsDiffer),
                                eir::Operand::reg(remainderNegative),
                                eir::Operand::reg(divisorNegative)));
    const std::uint32_t adjust = newRegister(bit);
    emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(adjust),
                                eir::Operand::reg(nonZero), eir::Operand::reg(signsDiffer)));
    const std::uint32_t adjusted = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::Add, eir::Operand::reg(adjusted),
                                eir::Operand::reg(remainder), eir::Operand::reg(divisor.reg)));
    const std::uint32_t result = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::Conditional, eir::Operand::reg(result),
                                eir::Operand::reg(adjust), eir::Operand::reg(adjusted),
                                eir::Operand::reg(remainder)));
    return Value{.reg = result, .type = type, .vhdlType = dividend.vhdlType};
}

std::optional<ProcessTranslator::Value> ProcessTranslator::shortCircuit(tree_t expression,
                                                                        subprogram_kind_t kind) {
    const std::optional<Value> left = this->expression(tree_value(tree_param(expression, 0)));
    if (!left) return std::nullopt;

    // `and` is decided by a false left operand and `or` by a true one; `nand` and `nor` are the
    // same tests with the result inverted.
    const bool decidedByTrue = kind == S_SCALAR_OR || kind == S_SCALAR_NOR;
    const bool inverted = kind == S_SCALAR_NAND || kind == S_SCALAR_NOR;

    const Value result{
        .reg = newRegister(left->type), .type = left->type, .vhdlType = tree_type(expression)};
    const std::uint32_t decided = newBlock("decided");
    const std::uint32_t evaluate = newBlock("evaluate-right");
    const std::uint32_t join = newBlock("short-circuit-join");
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(left->reg),
                                eir::Operand::block(decidedByTrue ? decided : evaluate),
                                eir::Operand::block(decidedByTrue ? evaluate : decided)));

    const auto store = [this, &result, inverted](const Value& value) {
        if (!inverted) {
            emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(result.reg),
                                        eir::Operand::reg(value.reg)));
            return;
        }
        const Value one = constant(1, value.type, nullptr);
        emit(eir::Instruction::make(eir::Opcode::Xor, eir::Operand::reg(result.reg),
                                    eir::Operand::reg(value.reg), eir::Operand::reg(one.reg)));
    };

    selectBlock(decided);
    store(*left);
    jump(join);

    selectBlock(evaluate);
    const std::optional<Value> right = this->expression(tree_value(tree_param(expression, 1)));
    if (!right) return std::nullopt;
    store(*right);
    jump(join);

    selectBlock(join);
    return result;
}

std::optional<ProcessTranslator::Value> ProcessTranslator::predefined(tree_t expression,
                                                                      subprogram_kind_t kind) {
    type_t resultType = tree_type(expression);
    switch (kind) {
    case S_SCALAR_AND:
    case S_SCALAR_OR:
    case S_SCALAR_NAND:
    case S_SCALAR_NOR: return shortCircuit(expression, kind);
    default: break;
    }

    std::vector<Value> operands;
    const unsigned count = tree_params(expression);
    for (unsigned index = 0; index < count; ++index) {
        const std::optional<Value> operand =
            this->expression(tree_value(tree_param(expression, index)));
        if (!operand) return std::nullopt;
        operands.push_back(*operand);
    }
    const auto unary = operands.size() == 1;
    const auto binary = operands.size() == 2;

    const std::optional<eir::Type> resultStorage = storageOf(resultType);
    const auto arithmetic = [&](eir::Opcode opcode) -> std::optional<Value> {
        if (!binary || !resultStorage) return std::nullopt;
        const std::uint32_t width = resultStorage->bitWidth();
        const Value left = resize(operands[0], width);
        const Value right = resize(operands[1], width);
        const std::uint32_t reg = newRegister(*resultStorage);
        emit(resultStorage->isSigned()
                 ? eir::Instruction::makeSigned(opcode, eir::Operand::reg(reg),
                                                eir::Operand::reg(left.reg),
                                                eir::Operand::reg(right.reg))
                 : eir::Instruction::make(opcode, eir::Operand::reg(reg),
                                          eir::Operand::reg(left.reg),
                                          eir::Operand::reg(right.reg)));
        return Value{.reg = reg, .type = *resultStorage, .vhdlType = resultType};
    };

    // Comparisons of two values of one type, in that type's storage.
    const auto comparison = [&](eir::Opcode opcode, bool swapped) -> std::optional<Value> {
        if (!binary) return std::nullopt;
        const std::uint32_t width =
            std::max(operands[0].type.bitWidth(), operands[1].type.bitWidth());
        const Value left = resize(operands[swapped ? 1 : 0], width);
        const Value right = resize(operands[swapped ? 0 : 1], width);
        return booleanOf(compare(opcode, left, right), resultType);
    };

    std::optional<Value> result;
    switch (kind) {
    case S_ADD: result = arithmetic(eir::Opcode::Add); break;
    case S_SUB: result = arithmetic(eir::Opcode::Subtract); break;
    case S_MUL:
    case S_MUL_PI:
    case S_MUL_IP: result = arithmetic(eir::Opcode::Multiply); break;
    case S_DIV:
    case S_DIV_PI: result = arithmetic(eir::Opcode::Divide); break;
    case S_REM: result = arithmetic(eir::Opcode::Modulo); break;

    // A time divided by a time is an integer, computed in the times' 64 bits.
    case S_DIV_PP:
        if (binary && resultStorage) {
            const std::uint32_t quotient = newRegister(operands[0].type);
            emit(eir::Instruction::makeSigned(eir::Opcode::Divide, eir::Operand::reg(quotient),
                                              eir::Operand::reg(operands[0].reg),
                                              eir::Operand::reg(operands[1].reg)));
            result = resize(Value{.reg = quotient, .type = operands[0].type, .vhdlType = nullptr},
                            resultStorage->bitWidth());
            result->vhdlType = resultType;
        }
        break;

    case S_MOD:
        if (binary && resultStorage)
            result = modulo(resize(operands[0], resultStorage->bitWidth()),
                            resize(operands[1], resultStorage->bitWidth()));
        break;

    case S_IDENTITY:
        if (unary) result = operands[0];
        break;

    case S_NEGATE:
        if (unary) {
            const std::uint32_t reg = newRegister(operands[0].type);
            emit(eir::Instruction::make(eir::Opcode::Negate, eir::Operand::reg(reg),
                                        eir::Operand::reg(operands[0].reg)));
            result = Value{.reg = reg, .type = operands[0].type, .vhdlType = resultType};
        }
        break;

    case S_ABS:
        if (unary) {
            const Value& value = operands[0];
            const std::uint32_t negated = newRegister(value.type);
            emit(eir::Instruction::make(eir::Opcode::Negate, eir::Operand::reg(negated),
                                        eir::Operand::reg(value.reg)));
            const std::uint32_t negative =
                compare(eir::Opcode::LessThan, value, constant(0, value.type, nullptr));
            const std::uint32_t reg = newRegister(value.type);
            emit(eir::Instruction::make(eir::Opcode::Conditional, eir::Operand::reg(reg),
                                        eir::Operand::reg(negative), eir::Operand::reg(negated),
                                        eir::Operand::reg(value.reg)));
            result = Value{.reg = reg, .type = value.type, .vhdlType = resultType};
        }
        break;

    case S_SCALAR_EQ: result = comparison(eir::Opcode::Equal, false); break;
    case S_SCALAR_NEQ: result = comparison(eir::Opcode::NotEqual, false); break;
    case S_SCALAR_LT: result = comparison(eir::Opcode::LessThan, false); break;
    case S_SCALAR_LE: result = comparison(eir::Opcode::LessEqual, false); break;
    case S_SCALAR_GT: result = comparison(eir::Opcode::LessThan, true); break;
    case S_SCALAR_GE: result = comparison(eir::Opcode::LessEqual, true); break;

    // On `bit` and `boolean`, whose codes are 0 and 1: the bitwise operation is the logical one,
    // and negation flips the low bit rather than every bit of the byte.
    case S_SCALAR_XOR:
    case S_SCALAR_XNOR:
        if (binary) {
            const std::uint32_t reg = newRegister(operands[0].type);
            emit(eir::Instruction::make(eir::Opcode::Xor, eir::Operand::reg(reg),
                                        eir::Operand::reg(operands[0].reg),
                                        eir::Operand::reg(operands[1].reg)));
            result = Value{.reg = reg, .type = operands[0].type, .vhdlType = resultType};
            if (kind == S_SCALAR_XNOR) {
                const Value one = constant(1, operands[0].type, nullptr);
                const std::uint32_t inverted = newRegister(operands[0].type);
                emit(eir::Instruction::make(eir::Opcode::Xor, eir::Operand::reg(inverted),
                                            eir::Operand::reg(reg), eir::Operand::reg(one.reg)));
                result->reg = inverted;
            }
        }
        break;

    case S_SCALAR_NOT:
        if (unary) {
            const Value one = constant(1, operands[0].type, nullptr);
            const std::uint32_t reg = newRegister(operands[0].type);
            emit(eir::Instruction::make(eir::Opcode::Xor, eir::Operand::reg(reg),
                                        eir::Operand::reg(operands[0].reg),
                                        eir::Operand::reg(one.reg)));
            result = Value{.reg = reg, .type = operands[0].type, .vhdlType = resultType};
        }
        break;

    case S_MINIMUM:
    case S_MAXIMUM:
        if (binary) {
            const std::uint32_t width =
                std::max(operands[0].type.bitWidth(), operands[1].type.bitWidth());
            const Value left = resize(operands[0], width);
            const Value right = resize(operands[1], width);
            const std::uint32_t less = compare(eir::Opcode::LessThan, left, right);
            const std::uint32_t reg = newRegister(left.type);
            const bool minimum = kind == S_MINIMUM;
            emit(eir::Instruction::make(eir::Opcode::Conditional, eir::Operand::reg(reg),
                                        eir::Operand::reg(less),
                                        eir::Operand::reg(minimum ? left.reg : right.reg),
                                        eir::Operand::reg(minimum ? right.reg : left.reg)));
            result = Value{.reg = reg, .type = left.type, .vhdlType = resultType};
        }
        break;

    default: break;
    }

    if (!result) {
        fail(expression, std::string("the operator ") + istr(tree_ident(expression)) + " on "
                             + type_pp(resultType));
    }
    return result;
}

} // namespace enzan::frontend::vhdl
