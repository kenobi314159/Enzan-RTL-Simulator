// SPDX-License-Identifier: GPL-3.0-or-later
//
// Concurrent assertions (IEEE 1800 section 16), of the properties every attempt of which ends a
// fixed number of clock ticks after it starts: booleans joined by `##n` and repeated by `[*n]`,
// under one `|->` or `|=>`, with `disable iff` and the sampled value functions.
//
// Such a property is a list of booleans, each checked a fixed number of ticks into an attempt, so
// an attempt is one bit saying whether it is still in flight, and the assertion one process
// keeping a bit for each tick an attempt can last. At each tick of its clock a new attempt
// starts, and every attempt checks the booleans its age reaches: one of the antecedent false
// ends it vacuously, one of the rest false fails it, and the last true passes it. The booleans
// read the values the objects had as the time slot began, which the module keeps in copies of
// them, so the process can run in the Reactive region, where the action blocks belong, and see
// the slot's non-blocking assignments there as an action block must.

#include "frontend/sv/ProcessTranslator.h"

#include "eir/BitVector.h"
#include "eir/Instruction.h"

#include "slang/ast/Compilation.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/syntax/SyntaxNode.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace enzan::frontend::sv {

namespace ast = slang::ast;

namespace {

/// The most ticks an attempt may last: each is a register, and a property lasting longer is far
/// more likely a mistake than a design.
constexpr std::uint32_t MaxPropertyLength = 65536;

const eir::Type Bit = eir::Type::bits(1, false, false);

} // namespace

std::optional<std::uint32_t> ProcessTranslator::readable(std::uint32_t objectIndex) {
    if (!sampling) return objectIndex;
    return module->sampledCopyOf(objectIndex);
}

bool ProcessTranslator::translateCopyInto(std::uint32_t target, std::uint32_t source) {
    const eir::Type type = module->definition().objects[source].type;
    const std::uint32_t value = newRegister(type);
    noteRead(source);
    emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(value),
                                eir::Operand::object(source)));
    noteWholeWrite(target);
    emit(eir::Instruction::make(eir::Opcode::Store, {}, eir::Operand::object(target),
                                eir::Operand::reg(value)));
    emit(eir::Instruction::make(eir::Opcode::Return));
    return !failed;
}

std::uint32_t ProcessTranslator::holds(const Value& value) {
    Value truth = truthOf(value);
    if (truth.type.bitWidth() > 1) {
        const std::uint32_t any = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::ReduceOr, eir::Operand::reg(any),
                                    eir::Operand::reg(truth.reg)));
        truth = Value{.reg = any, .type = eir::Type::logic()};
    }
    const std::uint32_t one = constantRegister(eir::Logic::One);
    const std::uint32_t result = newRegister(Bit);
    emit(eir::Instruction::make(eir::Opcode::CaseEqual, eir::Operand::reg(result),
                                eir::Operand::reg(truth.reg), eir::Operand::reg(one)));
    return result;
}

std::optional<std::uint32_t> ProcessTranslator::flattenSequence(const ast::AssertionExpr& sequence,
                                                                std::uint32_t offset,
                                                                bool antecedent,
                                                                std::vector<PropertyTerm>& terms) {
    const auto refuse = [&](std::string_view what) -> std::optional<std::uint32_t> {
        module->reportUnsupported(sequence.syntax != nullptr
                                      ? sequence.syntax->sourceRange().start()
                                      : slang::SourceLocation(),
                                  what);
        failed = true;
        return std::nullopt;
    };

    switch (sequence.kind) {
    case ast::AssertionExprKind::Simple: {
        const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
        // A named sequence's body, its arguments already in it.
        if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
            if (simple.repetition.has_value()) return refuse("a repetition of a named sequence");
            return flattenSequence(simple.expr.as<ast::AssertionInstanceExpression>().body, offset,
                                   antecedent, terms);
        }
        std::uint32_t count = 1;
        if (simple.repetition.has_value()) {
            const ast::SequenceRepetition& repetition = *simple.repetition;
            if (repetition.kind != ast::SequenceRepetition::Consecutive
                || repetition.range.max != repetition.range.min || repetition.range.min == 0)
                return refuse("a repetition other than a consecutive one of a fixed count of one "
                              "or more, such as [*3]");
            count = repetition.range.min;
            if (count > MaxPropertyLength - offset)
                return refuse(
                    std::format("a property lasting more than {} ticks", MaxPropertyLength));
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            if (index > 0) ++offset;
            terms.push_back(PropertyTerm{
                .offset = offset, .expression = &simple.expr, .antecedent = antecedent});
        }
        return offset;
    }

    case ast::AssertionExprKind::SequenceConcat: {
        const auto& concatenation = sequence.as<ast::SequenceConcatExpr>();
        bool first = true;
        for (const ast::SequenceConcatExpr::Element& element : concatenation.elements) {
            if (element.delay.max != element.delay.min)
                return refuse("a delay of a range of ticks, such as ##[1:3]; a fixed one, such "
                              "as ##2, is translated");
            // `a ##0 b` checks both at one tick, and a leading `##1 a` starts one tick in.
            if (!first || element.delay.min != 0) offset += element.delay.min;
            first = false;
            if (offset > MaxPropertyLength)
                return refuse(
                    std::format("a property lasting more than {} ticks", MaxPropertyLength));
            const std::optional<std::uint32_t> end =
                flattenSequence(*element.sequence, offset, antecedent, terms);
            if (!end.has_value()) return std::nullopt;
            offset = *end;
        }
        return offset;
    }

    case ast::AssertionExprKind::Clocking: return refuse("a clock inside a property");
    case ast::AssertionExprKind::DisableIff: return refuse("a disable iff inside a property");
    case ast::AssertionExprKind::Unary: return refuse("this property operator, such as not");
    case ast::AssertionExprKind::Binary:
        return refuse("this operator inside a sequence; only one |-> or |=> at the top of a "
                      "property is translated");
    default: return refuse("this kind of sequence");
    }
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateSampledFunction(const ast::CallExpression& call,
                                            std::string_view name) {
    const auto refuse = [&](std::string_view what) -> std::optional<Value> {
        module->reportUnsupported(call.sourceRange.start(), what);
        failed = true;
        return std::nullopt;
    };
    if (histories == nullptr) return refuse(std::format("{} outside a concurrent assertion", name));
    const auto arguments = call.arguments();
    if (arguments.empty()) return refuse(std::format("{} with no argument", name));

    // `$sampled` is its operand, which an assertion reads sampled anyway.
    if (name == "$sampled") {
        if (arguments.size() != 1) return refuse("$sampled with more than one argument");
        return translateExpression(*arguments[0]);
    }

    std::uint32_t depth = 1;
    if (name == "$past") {
        if (arguments.size() > 2)
            return refuse("$past with a gating expression or a clock of its own");
        if (arguments.size() == 2) {
            const std::optional<std::int64_t> ticks = constantValueOf(*arguments[1]);
            if (!ticks.has_value() || *ticks < 1 || *ticks > MaxPropertyLength)
                return refuse("$past of a number of ticks other than a constant of one or more");
            depth = static_cast<std::uint32_t>(*ticks);
        }
    } else if (arguments.size() > 1) {
        return refuse(std::format("{} with a clock of its own", name));
    }

    const std::optional<eir::Type> type = module->convertType(*arguments[0]->type);
    if (!type.has_value() || type->isReal())
        return refuse(std::format("{} of a value of this type", name));

    // The history the process shifts at each tick; translated again there, it is the operand
    // now, and so here.
    SampledHistory history{.operand = arguments[0], .type = *type, .past = {}};
    for (std::uint32_t index = 0; index < depth; ++index)
        history.past.push_back(newRegister(*type));
    const std::vector<std::uint32_t> past = history.past;
    histories->push_back(std::move(history));

    std::vector<SampledHistory>* const outer = histories;
    histories = nullptr; // A sampled value function of one is not translated.
    const std::optional<Value> operand = translateExpression(*arguments[0]);
    histories = outer;
    if (!operand.has_value()) return std::nullopt;
    const Value now = coerce(*operand, *type);
    const Value before{.reg = past.back(), .type = *type};
    if (name == "$past") return Value{.reg = past.back(), .type = *type};
    if (name == "$stable") return compare(eir::Opcode::CaseEqual, now, before);
    if (name == "$changed") return compare(eir::Opcode::CaseNotEqual, now, before);

    // `$rose` and `$fell` look at the least significant bit alone (IEEE 1800 section 16.9.3).
    const eir::Type lowType = eir::Type::bits(1, false, type->isFourState());
    const Value low = bitsOf(now, 0, lowType);
    const Value lowBefore = bitsOf(before, 0, lowType);
    const Value target =
        Value{.reg = constantRegister(name == "$rose" ? eir::Logic::One : eir::Logic::Zero),
              .type = eir::Type::logic()};
    const Value reached = compare(eir::Opcode::CaseEqual, low, target);
    const Value was = compare(eir::Opcode::CaseEqual, lowBefore, target);
    const std::uint32_t wasNot = newRegister(eir::Type::logic());
    emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(wasNot),
                                eir::Operand::reg(was.reg)));
    const std::uint32_t result = newRegister(eir::Type::logic());
    emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(result),
                                eir::Operand::reg(reached.reg), eir::Operand::reg(wasNot)));
    return Value{.reg = result, .type = eir::Type::logic()};
}

bool ProcessTranslator::translateConcurrentAssertion(const ast::Statement& statement,
                                                     const ast::Scope& scope) {
    const auto& assertion = statement.as<ast::ConcurrentAssertionStatement>();
    const auto refuse = [&](std::string_view what) {
        module->reportUnsupported(statement.sourceRange.start(), what);
        failed = true;
        return false;
    };
    const bool cover = assertion.assertionKind == ast::AssertionKind::CoverProperty;
    if (!cover && assertion.assertionKind != ast::AssertionKind::Assert
        && assertion.assertionKind != ast::AssertionKind::Assume)
        return refuse("this kind of concurrent assertion, such as restrict or cover sequence");

    // The clock and the disable condition around the property, a named property's among them,
    // or else the scope's defaults (IEEE 1800 sections 16.14.5 and 16.15).
    const ast::AssertionExpr* property = &assertion.propertySpec;
    const ast::TimingControl* clock = nullptr;
    const ast::Expression* disable = nullptr;
    while (true) {
        if (property->kind == ast::AssertionExprKind::Clocking && clock == nullptr) {
            const auto& clocking = property->as<ast::ClockingAssertionExpr>();
            clock = &clocking.clocking;
            property = &clocking.expr;
        } else if (property->kind == ast::AssertionExprKind::DisableIff && disable == nullptr) {
            const auto& disabled = property->as<ast::DisableIffAssertionExpr>();
            disable = &disabled.condition;
            property = &disabled.expr;
        } else if (property->kind == ast::AssertionExprKind::Simple
                   && !property->as<ast::SimpleAssertionExpr>().repetition.has_value()
                   && property->as<ast::SimpleAssertionExpr>().expr.kind
                          == ast::ExpressionKind::AssertionInstance) {
            property = &property->as<ast::SimpleAssertionExpr>()
                            .expr.as<ast::AssertionInstanceExpression>()
                            .body;
        } else {
            break;
        }
    }
    const slang::ast::Compilation& compilation = scope.getCompilation();
    if (clock == nullptr) {
        if (const ast::Symbol* clocking = compilation.getDefaultClocking(scope))
            clock = &clocking->as<ast::ClockingBlockSymbol>().getEvent();
    }
    if (disable == nullptr) disable = compilation.getDefaultDisable(scope);
    if (clock == nullptr) return refuse("a concurrent assertion with no clock");
    if (clock->kind != ast::TimingControlKind::SignalEvent
        || clock->as<ast::SignalEventControl>().iffCondition != nullptr)
        return refuse("a concurrent assertion's clock other than an edge of one expression");
    const auto& clockEvent = clock->as<ast::SignalEventControl>();

    // The booleans, each at its tick of an attempt.
    std::vector<PropertyTerm> terms;
    if (property->kind == ast::AssertionExprKind::Binary) {
        const auto& binary = property->as<ast::BinaryAssertionExpr>();
        const bool overlapped = binary.op == ast::BinaryAssertionOperator::OverlappedImplication;
        if (!overlapped && binary.op != ast::BinaryAssertionOperator::NonOverlappedImplication)
            return refuse("this property operator; |-> and |=> are translated");
        const std::optional<std::uint32_t> end = flattenSequence(binary.left, 0, true, terms);
        if (!end.has_value()) return false;
        if (!flattenSequence(binary.right, *end + (overlapped ? 0 : 1), false, terms).has_value())
            return false;
    } else if (!flattenSequence(*property, 0, false, terms).has_value()) {
        return false;
    }
    std::uint32_t length = 0;
    for (const PropertyTerm& term : terms) length = std::max(length, term.offset);

    process.reactive = true;

    // What the process wakes on: any change of the clock and of the disable condition, the edge
    // being its own to tell, since it wakes for either.
    std::vector<eir::SensitivityEntry> wakes;
    for (const ast::Expression* watched : {&clockEvent.expr, disable}) {
        if (watched == nullptr) continue;
        std::optional<eir::SensitivityEntry> entry = eventEntry(*watched, ast::EdgeKind::None);
        if (!entry.has_value()) entry = eventValueEntry(*watched, ast::EdgeKind::None);
        if (!entry.has_value()) {
            if (!failed)
                refuse("a concurrent assertion whose clock or disable condition reads a "
                       "variable of its own");
            return false;
        }
        wakes.push_back(*entry);
    }

    const auto move = [&](std::uint32_t to, std::uint32_t from) {
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(to),
                                    eir::Operand::reg(from)));
    };
    const auto binary = [&](eir::Opcode opcode, std::uint32_t left, std::uint32_t right) {
        const std::uint32_t result = newRegister(Bit);
        emit(eir::Instruction::make(opcode, eir::Operand::reg(result), eir::Operand::reg(left),
                                    eir::Operand::reg(right)));
        return result;
    };
    const auto inverse = [&](std::uint32_t bit) {
        const std::uint32_t result = newRegister(Bit);
        emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(result),
                                    eir::Operand::reg(bit)));
        return result;
    };
    const auto zero = [&] {
        const std::uint32_t result = newRegister(Bit);
        emit(eir::Instruction::make(
            eir::Opcode::LoadConstant, eir::Operand::reg(result),
            eir::Operand::constant(addConstant(eir::BitVector::fromUInt64(0, 1)))));
        return result;
    };
    const auto clockBit = [&]() -> std::optional<Value> {
        const std::optional<Value> value = translateExpression(clockEvent.expr);
        if (!value.has_value()) return std::nullopt;
        return bitsOf(*value, 0, eir::Type::logic());
    };
    // Runs `action` where `condition` holds, and goes on after it either way.
    const auto when = [&](std::uint32_t condition, const std::string& label,
                          const auto& action) -> bool {
        const std::uint32_t taken = newBlock(label);
        const std::uint32_t after = newBlock(label + "-done");
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition),
                                    eir::Operand::block(taken), eir::Operand::block(after)));
        selectBlock(taken);
        if (!action()) return false;
        if (!currentBlockTerminated())
            emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(after)));
        selectBlock(after);
        return true;
    };
    const auto runAction = [&](const ast::Statement* action) {
        return action == nullptr || translateStatement(*action);
    };
    const auto reportFailure = [&] {
        if (assertion.ifFalse != nullptr) return translateStatement(*assertion.ifFalse);
        // With no action of its own, a failing assertion is an `$error` (IEEE 1800
        // section 16.14.1).
        eir::SysTaskCall report;
        report.kind = eir::SysTaskKind::Error;
        report.formatIndex = addString(std::string(
            assertion.assertionKind == ast::AssertionKind::Assume ? "assumption failed"
                                                                  : "assertion failed"));
        report.location = module->convertLocation(statement.sourceRange.start());
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(report)))));
        return true;
    };

    // Before the first tick: no attempt in flight, the clock as it is, and each history its
    // operand's value now, as the value before the first tick is (IEEE 1800 section 16.9.3).
    std::vector<std::uint32_t> alive;
    alive.reserve(length + 1);
    for (std::uint32_t age = 0; age <= length; ++age) {
        alive.push_back(newRegister(Bit));
        move(alive.back(), zero());
    }
    const std::optional<Value> initialClock = clockBit();
    if (!initialClock.has_value()) return false;
    const std::uint32_t previousClock = newRegister(eir::Type::logic());
    move(previousClock, initialClock->reg);
    const std::uint32_t startHistories = newBlock("assert-start");
    const std::uint32_t waiting = newBlock("assert-wait");
    const std::uint32_t woken = newBlock("assert-woken");
    const std::uint32_t tick = newBlock("assert-tick");
    const std::uint32_t update = newBlock("assert-update");
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(startHistories)));

    selectBlock(waiting);
    const auto list = static_cast<std::uint32_t>(process.eventLists.size());
    process.eventLists.push_back(std::move(wakes));
    emit(eir::Instruction::make(eir::Opcode::WaitEvents, {}, eir::Operand::immediate(list),
                                eir::Operand::block(woken)));

    // Woken: whether the clock made its edge (IEEE 1800 section 9.4.2), and whether the property
    // is disabled, which ends every attempt in flight, whenever it comes.
    selectBlock(woken);
    const std::optional<Value> clockNow = clockBit();
    if (!clockNow.has_value()) return false;
    const auto is = [&](std::uint32_t bit, eir::Logic logic) {
        return binary(eir::Opcode::CaseEqual, bit, constantRegister(logic));
    };
    const std::uint32_t risen =
        binary(eir::Opcode::Or,
               binary(eir::Opcode::And, is(previousClock, eir::Logic::Zero),
                      inverse(is(clockNow->reg, eir::Logic::Zero))),
               binary(eir::Opcode::And, inverse(is(previousClock, eir::Logic::One)),
                      is(clockNow->reg, eir::Logic::One)));
    const std::uint32_t fallen =
        binary(eir::Opcode::Or,
               binary(eir::Opcode::And, is(previousClock, eir::Logic::One),
                      inverse(is(clockNow->reg, eir::Logic::One))),
               binary(eir::Opcode::And, inverse(is(previousClock, eir::Logic::Zero)),
                      is(clockNow->reg, eir::Logic::Zero)));
    std::uint32_t edge = 0;
    switch (clockEvent.edge) {
    case ast::EdgeKind::PosEdge: edge = risen; break;
    case ast::EdgeKind::NegEdge: edge = fallen; break;
    case ast::EdgeKind::BothEdges: edge = binary(eir::Opcode::Or, risen, fallen); break;
    case ast::EdgeKind::None:
        edge = binary(eir::Opcode::CaseNotEqual, previousClock, clockNow->reg);
        break;
    }
    move(previousClock, clockNow->reg);
    std::uint32_t enabled = 0;
    if (disable != nullptr) {
        const std::optional<Value> condition = translateExpression(*disable);
        if (!condition.has_value()) return false;
        const std::uint32_t disabled = holds(*condition);
        if (!when(disabled, "assert-disabled", [&] {
                for (const std::uint32_t attempt : alive) move(attempt, zero());
                return true;
            }))
            return false;
        enabled = inverse(disabled);
    }
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(edge),
                                eir::Operand::block(tick), eir::Operand::block(waiting)));

    // A tick: unless disabled, an attempt starts, and each in flight checks the booleans its age
    // reaches, the oldest first; an antecedent's before the rest at the same tick.
    selectBlock(tick);
    std::vector<SampledHistory> kept;
    histories = &kept;
    const std::uint32_t checks = newBlock("assert-checks");
    if (disable != nullptr) {
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(enabled),
                                    eir::Operand::block(checks), eir::Operand::block(update)));
    } else {
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(checks)));
    }
    selectBlock(checks);
    move(alive.front(), [&] {
        const std::uint32_t one = newRegister(Bit);
        emit(eir::Instruction::make(
            eir::Opcode::LoadConstant, eir::Operand::reg(one),
            eir::Operand::constant(addConstant(eir::BitVector::fromUInt64(1, 1)))));
        return one;
    }());
    for (std::uint32_t age = length + 1; age-- > 0;) {
        for (const bool antecedent : {true, false}) {
            for (const PropertyTerm& term : terms) {
                if (term.offset != age || term.antecedent != antecedent) continue;
                sampling = true;
                const std::optional<Value> value = translateExpression(*term.expression);
                sampling = false;
                if (!value.has_value()) return false;
                const std::uint32_t ok = holds(*value);
                const std::uint32_t missed = binary(eir::Opcode::And, alive[age], inverse(ok));
                if (antecedent) {
                    // A vacuous success, which runs the pass action as any success does.
                    if (!cover && assertion.ifTrue != nullptr
                        && !when(missed, "assert-vacuous",
                                 [&] { return runAction(assertion.ifTrue); }))
                        return false;
                } else if (!cover && !when(missed, "assert-fail", reportFailure)) {
                    return false;
                }
                move(alive[age], binary(eir::Opcode::And, alive[age], ok));
            }
        }
        if (age == length
            && !when(alive[age], "assert-pass", [&] { return runAction(assertion.ifTrue); }))
            return false;
    }
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(update)));

    // After every tick, disabled or not: each history takes its operand's value, and every attempt
    // grows a tick older, the oldest leaving.
    selectBlock(update);
    histories = nullptr;
    for (const SampledHistory& history : kept) {
        sampling = true;
        const std::optional<Value> value = translateExpression(*history.operand);
        sampling = false;
        if (!value.has_value()) return false;
        const Value now = coerce(*value, history.type);
        for (std::size_t index = history.past.size(); index-- > 1;)
            move(history.past[index], history.past[index - 1]);
        move(history.past.front(), now.reg);
    }
    for (std::uint32_t age = length; age > 0; --age) move(alive[age], alive[age - 1]);
    move(alive.front(), zero());
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(waiting)));

    // Now that the histories are known, each starts at its operand's value as the run begins.
    selectBlock(startHistories);
    for (const SampledHistory& history : kept) {
        const std::optional<Value> value = translateExpression(*history.operand);
        if (!value.has_value()) return false;
        const Value now = coerce(*value, history.type);
        for (const std::uint32_t past : history.past) move(past, now.reg);
    }
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(waiting)));
    return !failed;
}

} // namespace enzan::frontend::sv
