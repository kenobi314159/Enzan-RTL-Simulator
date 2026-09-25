// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/sv/ProcessTranslator.h"

#include "eir/BitVector.h"
#include "eir/Instruction.h"
#include "frontend/sv/SvIntConversion.h"

#include "slang/ast/Expression.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/Operator.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/types/Type.h"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <utility>

namespace enzan::frontend::sv {

namespace ast = slang::ast;

namespace {

/// Maps a slang edge to Enzan's.
std::optional<eir::EdgeKind> toEdgeKind(ast::EdgeKind edge) {
    switch (edge) {
    case ast::EdgeKind::None: return eir::EdgeKind::AnyChange;
    case ast::EdgeKind::PosEdge: return eir::EdgeKind::Posedge;
    case ast::EdgeKind::NegEdge: return eir::EdgeKind::Negedge;
    case ast::EdgeKind::BothEdges: return eir::EdgeKind::AnyChange;
    }
    return std::nullopt;
}

} // namespace

ProcessTranslator::ProcessTranslator(ModuleContext& owner, eir::ProcessKind kind, std::string name,
                                     SourceLocation location)
    : module(&owner) {
    process.kind = kind;
    process.name = std::move(name);
    process.location = location;
    newBlock("entry");
    current = 0;
}

// ---------------------------------------------------------------------------
// Emission helpers
// ---------------------------------------------------------------------------

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

void ProcessTranslator::emit(const eir::Instruction& instruction) {
    process.blocks[current].instructions.push_back(instruction);
}

std::uint32_t ProcessTranslator::addConstant(const eir::BitVector& value) {
    process.constants.push_back(value);
    return static_cast<std::uint32_t>(process.constants.size() - 1);
}

std::uint32_t ProcessTranslator::addString(std::string text) {
    process.strings.push_back(std::move(text));
    return static_cast<std::uint32_t>(process.strings.size() - 1);
}

void ProcessTranslator::selectBlock(std::uint32_t block) {
    current = block;
}

bool ProcessTranslator::currentBlockTerminated() const {
    const eir::BasicBlock& block = process.blocks[current];
    return !block.instructions.empty() && eir::isTerminator(block.instructions.back().opcode);
}

void ProcessTranslator::noteRead(std::uint32_t objectIndex) {
    reads.push_back(objectIndex);
}

void ProcessTranslator::noteWrite(std::uint32_t objectIndex) {
    writes.push_back(objectIndex);
}

ProcessTranslator::Value ProcessTranslator::coerce(const Value& value, eir::Type type) {
    if (value.type.bitWidth() == type.bitWidth()) return {.reg = value.reg, .type = type};

    const std::uint32_t result = newRegister(type);
    eir::Instruction instruction = eir::Instruction::make(
        type.bitWidth() < value.type.bitWidth() ? eir::Opcode::Truncate : eir::Opcode::Extend,
        eir::Operand::reg(result), eir::Operand::reg(value.reg));
    instruction.isSigned = value.type.isSigned();
    emit(instruction);
    return {.reg = result, .type = type};
}

ProcessTranslator::Value ProcessTranslator::coerceState(const Value& value, eir::Type type) {
    if (type.isFourState()) return value;

    // Assigning an unknown value into a two-state object turns X and Z into zero, which
    // IEEE 1800 requires. Emitted unconditionally for a two-state target rather than only
    // when the source might be unknown, because proving it cannot be is the optimiser's
    // job and there is no optimiser yet.
    const std::uint32_t result = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::CoerceTwoState, eir::Operand::reg(result),
                                eir::Operand::reg(value.reg)));
    return {.reg = result, .type = type};
}

void ProcessTranslator::setSensitivity(std::vector<eir::SensitivityEntry> sensitivity) {
    process.sensitivity = std::move(sensitivity);
    for (const eir::SensitivityEntry& entry : process.sensitivity) noteRead(entry.objectIndex);
}

eir::Process ProcessTranslator::take() {
    std::ranges::sort(reads);
    reads.erase(std::ranges::unique(reads).begin(), reads.end());
    std::ranges::sort(writes);
    writes.erase(std::ranges::unique(writes).begin(), writes.end());

    process.readSet = reads;
    process.writeSet = writes;
    return std::move(process);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool ProcessTranslator::translateBody(const ast::Statement& body) {
    if (!translateStatement(body)) return false;

    if (!currentBlockTerminated()) emit(eir::Instruction::make(eir::Opcode::Return));

    return !failed;
}

bool ProcessTranslator::translateExpressionInto(std::uint32_t objectIndex,
                                                const ast::Expression& expression) {
    const std::optional<Value> value = translateExpression(expression);
    if (!value.has_value()) return false;

    const eir::Type objectType = module->definition().objects[objectIndex].type;
    const Value widened = coerce(*value, objectType);
    const Value stored = coerceState(widened, objectType);

    noteWrite(objectIndex);
    emit(eir::Instruction::make(eir::Opcode::Store, {}, eir::Operand::object(objectIndex),
                                eir::Operand::reg(stored.reg)));

    if (!currentBlockTerminated()) emit(eir::Instruction::make(eir::Opcode::Return));

    return !failed;
}

bool ProcessTranslator::translateObjectInto(const ast::Expression& target,
                                            std::uint32_t objectIndex) {
    const eir::Type objectType = module->definition().objects[objectIndex].type;
    const std::uint32_t source = newRegister(objectType);

    noteRead(objectIndex);
    emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(source),
                                eir::Operand::object(objectIndex)));

    if (!assignTo(target, Value{.reg = source, .type = objectType}, false)) return false;

    if (!currentBlockTerminated()) emit(eir::Instruction::make(eir::Opcode::Return));

    return !failed;
}

void ProcessTranslator::inferSensitivityFromReads() {
    std::ranges::sort(reads);
    reads.erase(std::ranges::unique(reads).begin(), reads.end());

    process.sensitivity.clear();
    process.sensitivity.reserve(reads.size());
    for (const std::uint32_t objectIndex : reads) {
        process.sensitivity.push_back(
            eir::SensitivityEntry{.objectIndex = objectIndex, .edge = eir::EdgeKind::AnyChange});
    }
}

bool ProcessTranslator::translateContinuousAssignment(const ast::Expression& assignment) {
    if (!translateExpression(assignment).has_value() && failed) return false;

    if (!currentBlockTerminated()) emit(eir::Instruction::make(eir::Opcode::Return));

    return !failed;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

bool ProcessTranslator::translateStatement(const ast::Statement& statement) {
    if (failed) return false;

    switch (statement.kind) {
    case ast::StatementKind::Empty:
    case ast::StatementKind::VariableDeclaration:
        // A declaration's storage is created on demand when something references it,
        // and an initialiser on a procedural variable is handled by the declaration's
        // own assignment expression.
        return true;

    case ast::StatementKind::List:
    case ast::StatementKind::Block: return translateBlock(statement);

    case ast::StatementKind::ExpressionStatement:
        return translateExpressionStatement(statement.as<ast::ExpressionStatement>().expr);

    case ast::StatementKind::Conditional: return translateConditional(statement);

    case ast::StatementKind::Case: return translateCase(statement);

    case ast::StatementKind::ForLoop: return translateForLoop(statement);

    case ast::StatementKind::WhileLoop: return translateWhileLoop(statement, true);

    case ast::StatementKind::DoWhileLoop: return translateWhileLoop(statement, false);

    case ast::StatementKind::RepeatLoop: return translateRepeatLoop(statement);

    case ast::StatementKind::ForeverLoop: return translateForeverLoop(statement);

    case ast::StatementKind::Timed: return translateTimed(statement);

    default:
        module->reportUnsupported(statement.sourceRange.start(),
                                  std::string(toString(statement.kind)) + " statements");
        failed = true;
        return false;
    }
}

bool ProcessTranslator::translateBlock(const ast::Statement& statement) {
    if (statement.kind == ast::StatementKind::Block)
        return translateStatement(statement.as<ast::BlockStatement>().body);

    for (const ast::Statement* child : statement.as<ast::StatementList>().list) {
        if (!translateStatement(*child)) return false;

        // Anything after a terminator in the same block is unreachable; a `return` or a
        // `break` inside a list would leave the rest dead.
        if (currentBlockTerminated()) break;
    }
    return true;
}

bool ProcessTranslator::translateConditional(const ast::Statement& statement) {
    const auto& conditional = statement.as<ast::ConditionalStatement>();

    if (conditional.conditions.size() != 1 || conditional.conditions[0].pattern != nullptr) {
        module->reportUnsupported(statement.sourceRange.start(),
                                  "pattern matching and multi-condition `if`");
        failed = true;
        return false;
    }

    const std::optional<Value> condition = translateExpression(*conditional.conditions[0].expr);
    if (!condition.has_value()) return false;

    const std::uint32_t thenBlock = newBlock("if-then");
    const std::uint32_t elseBlock = newBlock("if-else");
    const std::uint32_t joinBlock = newBlock("if-join");

    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                eir::Operand::block(thenBlock), eir::Operand::block(elseBlock)));

    selectBlock(thenBlock);
    if (!translateStatement(conditional.ifTrue)) return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(joinBlock)));

    selectBlock(elseBlock);
    if (conditional.ifFalse != nullptr && !translateStatement(*conditional.ifFalse)) return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(joinBlock)));

    selectBlock(joinBlock);
    return true;
}

bool ProcessTranslator::translateCase(const ast::Statement& statement) {
    const auto& caseStatement = statement.as<ast::CaseStatement>();

    if (caseStatement.condition != ast::CaseStatementCondition::Normal) {
        // `casez` and `casex` need the wildcard comparison of `==?`, which the value engine
        // does not implement yet. Reported rather than approximated with `==`, because an
        // approximated wildcard silently takes the wrong branch.
        module->reportUnsupported(statement.sourceRange.start(),
                                  "`casez`, `casex` and `case ... inside`");
        failed = true;
        return false;
    }

    const std::optional<Value> selector = translateExpression(caseStatement.expr);
    if (!selector.has_value()) return false;

    const std::uint32_t joinBlock = newBlock("case-join");

    // Each group gets a test chain and a body; the chain falls through to the next group's
    // test, and the last one falls through to the default.
    std::vector<std::uint32_t> bodyBlocks;
    bodyBlocks.reserve(caseStatement.items.size());
    for (std::size_t index = 0; index < caseStatement.items.size(); ++index)
        bodyBlocks.push_back(newBlock("case-body"));

    const std::uint32_t defaultBlock = newBlock("case-default");

    for (std::size_t index = 0; index < caseStatement.items.size(); ++index) {
        const ast::CaseStatement::ItemGroup& group = caseStatement.items[index];
        for (const ast::Expression* item : group.expressions) {
            const std::optional<Value> itemValue = translateExpression(*item);
            if (!itemValue.has_value()) return false;

            const std::uint32_t width =
                std::max(selector->type.bitWidth(), itemValue->type.bitWidth());
            const eir::Type comparisonType = eir::Type::bits(
                width, selector->type.isSigned() && itemValue->type.isSigned(), true);
            const Value left = coerce(*selector, comparisonType);
            const Value right = coerce(*itemValue, comparisonType);

            const std::uint32_t matched = newRegister(eir::Type::logic());
            emit(eir::Instruction::make(eir::Opcode::Equal, eir::Operand::reg(matched),
                                        eir::Operand::reg(left.reg), eir::Operand::reg(right.reg)));

            const std::uint32_t nextTest = newBlock("case-test");
            emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(matched),
                                        eir::Operand::block(bodyBlocks[index]),
                                        eir::Operand::block(nextTest)));
            selectBlock(nextTest);
        }
    }

    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(defaultBlock)));

    for (std::size_t index = 0; index < caseStatement.items.size(); ++index) {
        selectBlock(bodyBlocks[index]);
        if (!translateStatement(*caseStatement.items[index].stmt)) return false;
        if (!currentBlockTerminated())
            emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(joinBlock)));
    }

    selectBlock(defaultBlock);
    if (caseStatement.defaultCase != nullptr && !translateStatement(*caseStatement.defaultCase))
        return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(joinBlock)));

    selectBlock(joinBlock);
    return true;
}

bool ProcessTranslator::translateForLoop(const ast::Statement& statement) {
    const auto& loop = statement.as<ast::ForLoopStatement>();

    for (const ast::Expression* initializer : loop.initializers) {
        if (!translateExpression(*initializer).has_value() && failed) return false;
    }

    const std::uint32_t testBlock = newBlock("for-test");
    const std::uint32_t bodyBlock = newBlock("for-body");
    const std::uint32_t stepBlock = newBlock("for-step");
    const std::uint32_t exitBlock = newBlock("for-exit");

    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(testBlock)));

    selectBlock(testBlock);
    if (loop.stopExpr != nullptr) {
        const std::optional<Value> condition = translateExpression(*loop.stopExpr);
        if (!condition.has_value()) return false;
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(condition->reg),
                                    eir::Operand::block(bodyBlock),
                                    eir::Operand::block(exitBlock)));
    } else {
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(bodyBlock)));
    }

    selectBlock(bodyBlock);
    if (!translateStatement(loop.body)) return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(stepBlock)));

    selectBlock(stepBlock);
    for (const ast::Expression* step : loop.steps) {
        if (!translateExpression(*step).has_value() && failed) return false;
    }
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(testBlock)));

    selectBlock(exitBlock);
    return true;
}

bool ProcessTranslator::translateWhileLoop(const ast::Statement& statement, bool testFirst) {
    const ast::Expression* condition = nullptr;
    const ast::Statement* body = nullptr;

    if (testFirst) {
        const auto& loop = statement.as<ast::WhileLoopStatement>();
        condition = &loop.cond;
        body = &loop.body;
    } else {
        const auto& loop = statement.as<ast::DoWhileLoopStatement>();
        condition = &loop.cond;
        body = &loop.body;
    }

    const std::uint32_t testBlock = newBlock("while-test");
    const std::uint32_t bodyBlock = newBlock("while-body");
    const std::uint32_t exitBlock = newBlock("while-exit");

    // The only difference between `while` and `do ... while` is where control enters.
    emit(eir::Instruction::make(eir::Opcode::Jump, {},
                                eir::Operand::block(testFirst ? testBlock : bodyBlock)));

    selectBlock(testBlock);
    const std::optional<Value> test = translateExpression(*condition);
    if (!test.has_value()) return false;
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(test->reg),
                                eir::Operand::block(bodyBlock), eir::Operand::block(exitBlock)));

    selectBlock(bodyBlock);
    if (!translateStatement(*body)) return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(testBlock)));

    selectBlock(exitBlock);
    return true;
}

bool ProcessTranslator::translateRepeatLoop(const ast::Statement& statement) {
    const auto& loop = statement.as<ast::RepeatLoopStatement>();

    const std::optional<Value> count = translateExpression(loop.count);
    if (!count.has_value()) return false;

    // A counter register initialised from the count and decremented each iteration. The
    // count is evaluated once, before the loop, which is what IEEE 1800 requires: changing
    // the expression inside the body does not change the number of iterations.
    const eir::Type counterType = eir::Type::bits(count->type.bitWidth(), false, true);
    const std::uint32_t counter = newRegister(counterType);
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(counter),
                                eir::Operand::reg(count->reg)));

    const std::uint32_t one = addConstant(eir::BitVector::fromUInt64(1, counterType.bitWidth()));
    const std::uint32_t oneRegister = newRegister(counterType);
    const std::uint32_t zero =
        addConstant(eir::BitVector::filled(counterType.bitWidth(), eir::Logic::Zero));
    const std::uint32_t zeroRegister = newRegister(counterType);

    const std::uint32_t testBlock = newBlock("repeat-test");
    const std::uint32_t bodyBlock = newBlock("repeat-body");
    const std::uint32_t exitBlock = newBlock("repeat-exit");

    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(oneRegister),
                                eir::Operand::constant(one)));
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(zeroRegister),
                                eir::Operand::constant(zero)));
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(testBlock)));

    selectBlock(testBlock);
    const std::uint32_t remaining = newRegister(eir::Type::logic());
    emit(eir::Instruction::make(eir::Opcode::NotEqual, eir::Operand::reg(remaining),
                                eir::Operand::reg(counter), eir::Operand::reg(zeroRegister)));
    emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(remaining),
                                eir::Operand::block(bodyBlock), eir::Operand::block(exitBlock)));

    selectBlock(bodyBlock);
    if (!translateStatement(loop.body)) return false;
    if (!currentBlockTerminated()) {
        const std::uint32_t decremented = newRegister(counterType);
        emit(eir::Instruction::make(eir::Opcode::Subtract, eir::Operand::reg(decremented),
                                    eir::Operand::reg(counter), eir::Operand::reg(oneRegister)));
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(counter),
                                    eir::Operand::reg(decremented)));
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(testBlock)));
    }

    selectBlock(exitBlock);
    return true;
}

bool ProcessTranslator::translateForeverLoop(const ast::Statement& statement) {
    const auto& loop = statement.as<ast::ForeverLoopStatement>();

    const std::uint32_t bodyBlock = newBlock("forever-body");
    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(bodyBlock)));

    selectBlock(bodyBlock);
    if (!translateStatement(loop.body)) return false;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(bodyBlock)));

    // Nothing follows a `forever` loop, so a fresh block is created only so that the
    // caller has an unterminated one to continue in; it stays empty and is removed by
    // being unreachable. Emitting a Return keeps every block well formed.
    const std::uint32_t afterBlock = newBlock("forever-after");
    selectBlock(afterBlock);
    emit(eir::Instruction::make(eir::Opcode::Return));
    return true;
}

bool ProcessTranslator::translateTimed(const ast::Statement& statement) {
    const auto& timed = statement.as<ast::TimedStatement>();

    if (!translateTimingControl(timed.timing)) return false;

    return translateStatement(timed.stmt);
}

bool ProcessTranslator::translateTimingControl(const ast::TimingControl& timing) {
    switch (timing.kind) {
    case ast::TimingControlKind::Delay: {
        const auto& delay = timing.as<ast::DelayControl>();
        const std::optional<Value> amount = translateExpression(delay.expr);
        if (!amount.has_value()) return false;

        const std::uint32_t resume = newBlock("after-delay");
        emit(eir::Instruction::make(eir::Opcode::WaitTime, {}, eir::Operand::reg(amount->reg),
                                    eir::Operand::block(resume)));
        selectBlock(resume);
        return true;
    }

    case ast::TimingControlKind::SignalEvent: {
        const auto& event = timing.as<ast::SignalEventControl>();
        if (event.iffCondition != nullptr) {
            module->reportUnsupported(timing.sourceRange.start(), "`iff` on an event control");
            failed = true;
            return false;
        }

        if (event.expr.kind != ast::ExpressionKind::NamedValue
            && event.expr.kind != ast::ExpressionKind::HierarchicalValue) {
            module->reportUnsupported(timing.sourceRange.start(),
                                      "an event control on an expression rather than a signal");
            failed = true;
            return false;
        }

        const std::optional<std::uint32_t> objectIndex =
            module->objectFor(event.expr.as<ast::ValueExpressionBase>().symbol);
        if (!objectIndex.has_value()) {
            failed = true;
            return false;
        }

        const std::optional<eir::EdgeKind> edge = toEdgeKind(event.edge);
        if (!edge.has_value()) {
            module->reportUnsupported(timing.sourceRange.start(), "this edge specifier");
            failed = true;
            return false;
        }

        noteRead(*objectIndex);
        const std::uint32_t resume = newBlock("after-edge");
        emit(eir::Instruction::make(eir::Opcode::WaitEdge, {}, eir::Operand::object(*objectIndex),
                                    eir::Operand::immediate(static_cast<std::uint32_t>(*edge)),
                                    eir::Operand::block(resume)));
        selectBlock(resume);
        return true;
    }

    default:
        // An event list inside a body would need the scheduler to subscribe a process to
        // several signals at once and then unsubscribe it from all of them. That is
        // worth doing, but it is not the common case: an event list almost always sits
        // on the block header, where it becomes the process's static sensitivity.
        module->reportUnsupported(timing.sourceRange.start(),
                                  std::string(toString(timing.kind)) + " timing controls");
        failed = true;
        return false;
    }
}

bool ProcessTranslator::translateExpressionStatement(const ast::Expression& expression) {
    if (expression.kind == ast::ExpressionKind::Call
        && expression.as<ast::CallExpression>().isSystemCall() && expression.type->isVoid()) {
        return translateSystemTask(expression);
    }

    return translateExpression(expression).has_value() || !failed;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateExpression(const ast::Expression& expression) {
    if (failed) return std::nullopt;

    const std::optional<eir::Type> type = module->convertType(*expression.type);

    switch (expression.kind) {
    case ast::ExpressionKind::IntegerLiteral: {
        if (!type.has_value()) break;
        const std::uint32_t constant =
            addConstant(toBitVector(expression.as<ast::IntegerLiteral>().getValue()));
        const std::uint32_t result = newRegister(*type);
        eir::Instruction instruction = eir::Instruction::make(
            eir::Opcode::LoadConstant, eir::Operand::reg(result), eir::Operand::constant(constant));
        instruction.isSigned = type->isSigned();
        emit(instruction);
        return Value{.reg = result, .type = *type};
    }

    case ast::ExpressionKind::UnbasedUnsizedIntegerLiteral: {
        if (!type.has_value()) break;
        const std::uint32_t constant =
            addConstant(toBitVector(expression.as<ast::UnbasedUnsizedIntegerLiteral>().getValue()));
        const std::uint32_t result = newRegister(*type);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                    eir::Operand::constant(constant)));
        return Value{.reg = result, .type = *type};
    }

    case ast::ExpressionKind::StringLiteral: {
        const eir::BitVector packed = toBitVector(expression.as<ast::StringLiteral>().getValue());
        const eir::Type stringType = eir::Type::bits(packed.width(), false, false);
        const std::uint32_t constant = addConstant(packed);
        const std::uint32_t result = newRegister(stringType);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                    eir::Operand::constant(constant)));
        return Value{.reg = result, .type = stringType};
    }

    case ast::ExpressionKind::NamedValue:
    case ast::ExpressionKind::HierarchicalValue: {
        const ast::ValueSymbol& symbol = expression.as<ast::ValueExpressionBase>().symbol;

        // A parameter is a constant, not state: reading it must not create an object,
        // because an object would be a per-instance slot for a value that never changes.
        if (symbol.kind == slang::ast::SymbolKind::Parameter) {
            const slang::ConstantValue& value = symbol.as<ast::ParameterSymbol>().getValue();
            if (!value.isInteger() || !type.has_value()) break;
            const std::uint32_t constant = addConstant(toBitVector(value.integer()));
            const std::uint32_t result = newRegister(*type);
            emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                        eir::Operand::constant(constant)));
            return Value{.reg = result, .type = *type};
        }

        const std::optional<std::uint32_t> objectIndex = module->objectFor(symbol);
        if (!objectIndex.has_value()) break;

        const eir::Type objectType = module->definition().objects[*objectIndex].type;
        noteRead(*objectIndex);
        const std::uint32_t result = newRegister(objectType);
        emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(result),
                                    eir::Operand::object(*objectIndex)));
        return Value{.reg = result, .type = objectType};
    }

    case ast::ExpressionKind::LValueReference:
        // Stands for the current value of the assignment target in a compound
        // assignment. It appears only inside one, where the value has been read and
        // bound; reaching it elsewhere is a defect in this translator rather than in the
        // user's source.
        if (lvalueReference.has_value()) return lvalueReference;
        break;

    case ast::ExpressionKind::UnaryOp: return translateUnary(expression);

    case ast::ExpressionKind::BinaryOp: return translateBinary(expression);

    case ast::ExpressionKind::ConditionalOp: return translateConditionalExpression(expression);

    case ast::ExpressionKind::Assignment: return translateAssignment(expression);

    case ast::ExpressionKind::Concatenation: return translateConcatenation(expression);

    case ast::ExpressionKind::Replication: return translateReplication(expression);

    case ast::ExpressionKind::ElementSelect: return translateElementSelect(expression);

    case ast::ExpressionKind::RangeSelect: return translateRangeSelect(expression);

    case ast::ExpressionKind::Call: return translateCall(expression);

    case ast::ExpressionKind::Conversion: {
        // slang inserts a conversion wherever the language's width and signedness rules
        // change a value, so honouring it is what makes the widths downstream correct.
        const std::optional<Value> operand =
            translateExpression(expression.as<ast::ConversionExpression>().operand());
        if (!operand.has_value() || !type.has_value()) break;
        return coerce(*operand, *type);
    }

    default: break;
    }

    if (!failed) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  std::string(toString(expression.kind)) + " expressions");
        failed = true;
    }
    return std::nullopt;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateBinary(const ast::Expression& expression) {
    const auto& binary = expression.as<ast::BinaryExpression>();

    const std::optional<Value> left = translateExpression(binary.left());
    if (!left.has_value()) return std::nullopt;
    const std::optional<Value> right = translateExpression(binary.right());
    if (!right.has_value()) return std::nullopt;

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this operand type");
        failed = true;
        return std::nullopt;
    }

    const auto arithmetic = [&](eir::Opcode opcode) -> std::optional<Value> {
        const Value a = coerce(*left, *resultType);
        const Value b = coerce(*right, *resultType);
        const std::uint32_t result = newRegister(*resultType);
        eir::Instruction instruction = eir::Instruction::make(
            opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg), eir::Operand::reg(b.reg));
        instruction.isSigned = resultType->isSigned();
        emit(instruction);
        return Value{.reg = result, .type = *resultType};
    };

    // Comparison operands keep their own width, extended to whichever is wider, because the
    // result is one bit and the comparison itself decides the signedness.
    const auto comparison = [&](eir::Opcode opcode, bool swapOperands) -> std::optional<Value> {
        const bool isSigned = left->type.isSigned() && right->type.isSigned();
        const std::uint32_t width = std::max(left->type.bitWidth(), right->type.bitWidth());
        const eir::Type operandType = eir::Type::bits(width, isSigned, true);

        const Value a = coerce(swapOperands ? *right : *left, operandType);
        const Value b = coerce(swapOperands ? *left : *right, operandType);
        const std::uint32_t result = newRegister(eir::Type::logic());
        eir::Instruction instruction = eir::Instruction::make(
            opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg), eir::Operand::reg(b.reg));
        instruction.isSigned = isSigned;
        emit(instruction);
        return Value{.reg = result, .type = eir::Type::logic()};
    };

    const auto shift = [&](eir::Opcode opcode) -> std::optional<Value> {
        // The shift amount is self-determined: its width has nothing to do with the value
        // being shifted, which is why the bytecode carries a width per operand.
        const Value a = coerce(*left, *resultType);
        const std::uint32_t result = newRegister(*resultType);
        emit(eir::Instruction::make(opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg),
                                    eir::Operand::reg(right->reg)));
        return Value{.reg = result, .type = *resultType};
    };

    const auto logical = [&](eir::Opcode opcode) -> std::optional<Value> {
        const std::uint32_t result = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(opcode, eir::Operand::reg(result), eir::Operand::reg(left->reg),
                                    eir::Operand::reg(right->reg)));
        return Value{.reg = result, .type = eir::Type::logic()};
    };

    switch (binary.op) {
    case ast::BinaryOperator::Add: return arithmetic(eir::Opcode::Add);
    case ast::BinaryOperator::Subtract: return arithmetic(eir::Opcode::Subtract);
    case ast::BinaryOperator::Multiply: return arithmetic(eir::Opcode::Multiply);
    case ast::BinaryOperator::Divide: return arithmetic(eir::Opcode::Divide);
    case ast::BinaryOperator::Mod: return arithmetic(eir::Opcode::Modulo);
    case ast::BinaryOperator::BinaryAnd: return arithmetic(eir::Opcode::And);
    case ast::BinaryOperator::BinaryOr: return arithmetic(eir::Opcode::Or);
    case ast::BinaryOperator::BinaryXor: return arithmetic(eir::Opcode::Xor);
    case ast::BinaryOperator::BinaryXnor: return arithmetic(eir::Opcode::Xnor);

    case ast::BinaryOperator::Equality: return comparison(eir::Opcode::Equal, false);
    case ast::BinaryOperator::Inequality: return comparison(eir::Opcode::NotEqual, false);
    case ast::BinaryOperator::CaseEquality: return comparison(eir::Opcode::CaseEqual, false);
    case ast::BinaryOperator::CaseInequality: return comparison(eir::Opcode::CaseNotEqual, false);
    case ast::BinaryOperator::LessThan: return comparison(eir::Opcode::LessThan, false);
    case ast::BinaryOperator::LessThanEqual: return comparison(eir::Opcode::LessEqual, false);

    // `a > b` is `b < a`: one comparison opcode each way is enough, and swapping here
    // keeps the value engine's surface smaller.
    case ast::BinaryOperator::GreaterThan: return comparison(eir::Opcode::LessThan, true);
    case ast::BinaryOperator::GreaterThanEqual: return comparison(eir::Opcode::LessEqual, true);

    case ast::BinaryOperator::LogicalAnd: return logical(eir::Opcode::LogicalAnd);
    case ast::BinaryOperator::LogicalOr: return logical(eir::Opcode::LogicalOr);

    case ast::BinaryOperator::LogicalShiftLeft:
    case ast::BinaryOperator::ArithmeticShiftLeft:
        // `<<<` and `<<` differ only in how the standard describes them; both shift
        // zeroes in from the right.
        return shift(eir::Opcode::ShiftLeft);
    case ast::BinaryOperator::LogicalShiftRight: return shift(eir::Opcode::ShiftRightLogical);
    case ast::BinaryOperator::ArithmeticShiftRight:
        return shift(left->type.isSigned() ? eir::Opcode::ShiftRightArithmetic
                                           : eir::Opcode::ShiftRightLogical);

    default: break;
    }

    module->reportUnsupported(expression.sourceRange.start(), std::string("the `")
                                                                  + std::string(toString(binary.op))
                                                                  + "` operator");
    failed = true;
    return std::nullopt;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateUnary(const ast::Expression& expression) {
    const auto& unary = expression.as<ast::UnaryExpression>();

    // The increment and decrement operators read and write their operand, so they are
    // handled before the operand is translated as a plain value.
    switch (unary.op) {
    case ast::UnaryOperator::Preincrement:
    case ast::UnaryOperator::Predecrement:
    case ast::UnaryOperator::Postincrement:
    case ast::UnaryOperator::Postdecrement: {
        const std::optional<Value> before = translateExpression(unary.operand());
        if (!before.has_value()) return std::nullopt;

        const bool increment = unary.op == ast::UnaryOperator::Preincrement
                               || unary.op == ast::UnaryOperator::Postincrement;
        const std::uint32_t one =
            addConstant(eir::BitVector::fromUInt64(1, before->type.bitWidth()));
        const std::uint32_t oneRegister = newRegister(before->type);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(oneRegister),
                                    eir::Operand::constant(one)));

        const std::uint32_t updated = newRegister(before->type);
        emit(eir::Instruction::make(increment ? eir::Opcode::Add : eir::Opcode::Subtract,
                                    eir::Operand::reg(updated), eir::Operand::reg(before->reg),
                                    eir::Operand::reg(oneRegister)));

        const Value result{.reg = updated, .type = before->type};
        if (!assignTo(unary.operand(), result, false)) return std::nullopt;

        // `i++` yields the old value and `++i` the new one.
        const bool yieldsNew = unary.op == ast::UnaryOperator::Preincrement
                               || unary.op == ast::UnaryOperator::Predecrement;
        return yieldsNew ? result : *before;
    }

    default: break;
    }

    const std::optional<Value> operand = translateExpression(unary.operand());
    if (!operand.has_value()) return std::nullopt;

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this operand type");
        failed = true;
        return std::nullopt;
    }

    const auto elementwise = [&](eir::Opcode opcode) -> std::optional<Value> {
        const Value a = coerce(*operand, *resultType);
        const std::uint32_t result = newRegister(*resultType);
        emit(eir::Instruction::make(opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg)));
        return Value{.reg = result, .type = *resultType};
    };

    const auto reduction = [&](eir::Opcode opcode, bool invert) -> std::optional<Value> {
        const std::uint32_t reduced = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(opcode, eir::Operand::reg(reduced),
                                    eir::Operand::reg(operand->reg)));
        if (!invert) return Value{.reg = reduced, .type = eir::Type::logic()};

        const std::uint32_t inverted = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::Not, eir::Operand::reg(inverted),
                                    eir::Operand::reg(reduced)));
        return Value{.reg = inverted, .type = eir::Type::logic()};
    };

    switch (unary.op) {
    case ast::UnaryOperator::Plus: return coerce(*operand, *resultType);
    case ast::UnaryOperator::Minus: return elementwise(eir::Opcode::Negate);
    case ast::UnaryOperator::BitwiseNot: return elementwise(eir::Opcode::Not);

    case ast::UnaryOperator::BitwiseAnd: return reduction(eir::Opcode::ReduceAnd, false);
    case ast::UnaryOperator::BitwiseOr: return reduction(eir::Opcode::ReduceOr, false);
    case ast::UnaryOperator::BitwiseXor: return reduction(eir::Opcode::ReduceXor, false);
    case ast::UnaryOperator::BitwiseNand: return reduction(eir::Opcode::ReduceAnd, true);
    case ast::UnaryOperator::BitwiseNor: return reduction(eir::Opcode::ReduceOr, true);
    case ast::UnaryOperator::BitwiseXnor: return reduction(eir::Opcode::ReduceXor, true);

    case ast::UnaryOperator::LogicalNot: {
        const std::uint32_t result = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::LogicalNot, eir::Operand::reg(result),
                                    eir::Operand::reg(operand->reg)));
        return Value{.reg = result, .type = eir::Type::logic()};
    }

    default: break;
    }

    module->reportUnsupported(expression.sourceRange.start(), "this unary operator");
    failed = true;
    return std::nullopt;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateConditionalExpression(const ast::Expression& expression) {
    const auto& conditional = expression.as<ast::ConditionalExpression>();

    if (conditional.conditions.size() != 1 || conditional.conditions[0].pattern != nullptr) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  "pattern matching in a conditional expression");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Value> condition = translateExpression(*conditional.conditions[0].expr);
    if (!condition.has_value()) return std::nullopt;

    // Both arms are evaluated, because the result merges them per bit when the condition is
    // unknown (see BitOps::conditional). Short-circuiting would make `x ? a : b` unable to
    // produce the merge the standard requires.
    const std::optional<Value> whenTrue = translateExpression(conditional.left());
    if (!whenTrue.has_value()) return std::nullopt;
    const std::optional<Value> whenFalse = translateExpression(conditional.right());
    if (!whenFalse.has_value()) return std::nullopt;

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this conditional result type");
        failed = true;
        return std::nullopt;
    }

    const Value a = coerce(*whenTrue, *resultType);
    const Value b = coerce(*whenFalse, *resultType);
    const std::uint32_t result = newRegister(*resultType);
    emit(eir::Instruction::make(eir::Opcode::Conditional, eir::Operand::reg(result),
                                eir::Operand::reg(condition->reg), eir::Operand::reg(a.reg),
                                eir::Operand::reg(b.reg)));
    return Value{.reg = result, .type = *resultType};
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateConcatenation(const ast::Expression& expression) {
    const auto& concatenation = expression.as<ast::ConcatenationExpression>();

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this concatenation type");
        failed = true;
        return std::nullopt;
    }

    const std::uint32_t zero =
        addConstant(eir::BitVector::filled(resultType->bitWidth(), eir::Logic::Zero));
    const std::uint32_t result = newRegister(*resultType);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                eir::Operand::constant(zero)));

    // Operands are written most significant first, which is the order they appear in.
    std::uint32_t offset = resultType->bitWidth();
    for (const ast::Expression* operand : concatenation.operands()) {
        const std::optional<Value> value = translateExpression(*operand);
        if (!value.has_value()) return std::nullopt;

        if (value->type.bitWidth() > offset) {
            module->reportUnsupported(expression.sourceRange.start(),
                                      "a concatenation whose operands exceed its width");
            failed = true;
            return std::nullopt;
        }
        offset -= value->type.bitWidth();

        const std::uint32_t offsetConstant = addConstant(eir::BitVector::fromUInt64(offset, 32));
        const std::uint32_t offsetRegister = newRegister(eir::Type::bits(32, false, false));
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(offsetRegister),
                                    eir::Operand::constant(offsetConstant)));
        emit(eir::Instruction::make(eir::Opcode::SplicePart, eir::Operand::reg(result),
                                    eir::Operand::reg(value->reg),
                                    eir::Operand::reg(offsetRegister)));
    }

    return Value{.reg = result, .type = *resultType};
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateReplication(const ast::Expression& expression) {
    const auto& replication = expression.as<ast::ReplicationExpression>();

    const std::optional<std::int64_t> count = constantValueOf(replication.count());
    if (!count.has_value() || *count < 0) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  "a replication whose count is not a constant");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Value> inner = translateExpression(replication.concat());
    if (!inner.has_value()) return std::nullopt;

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this replication type");
        failed = true;
        return std::nullopt;
    }

    const std::uint32_t zero =
        addConstant(eir::BitVector::filled(resultType->bitWidth(), eir::Logic::Zero));
    const std::uint32_t result = newRegister(*resultType);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                eir::Operand::constant(zero)));

    for (std::int64_t repetition = 0; repetition < *count; ++repetition) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(repetition) * inner->type.bitWidth();
        const std::uint32_t offsetConstant = addConstant(eir::BitVector::fromUInt64(offset, 32));
        const std::uint32_t offsetRegister = newRegister(eir::Type::bits(32, false, false));
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(offsetRegister),
                                    eir::Operand::constant(offsetConstant)));
        emit(eir::Instruction::make(eir::Opcode::SplicePart, eir::Operand::reg(result),
                                    eir::Operand::reg(inner->reg),
                                    eir::Operand::reg(offsetRegister)));
    }

    return Value{.reg = result, .type = *resultType};
}

std::uint32_t ProcessTranslator::elementBitsOf(const ast::Type& type) {
    const slang::ConstantRange range = type.getFixedRange();
    const std::uint32_t elements = range.width();
    const std::uint32_t total = type.getBitWidth();

    // A ragged or zero-element integral type cannot occur, but dividing by zero here would
    // be far worse than a conservative answer.
    if (elements == 0 || total % elements != 0) return 1;

    return total / elements;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::scaleIndexToBitOffset(const Value& index, std::int32_t rangeLow,
                                         std::uint32_t elementBits) {
    const Value shifted = shiftIndexToBitOffset(index, rangeLow);
    if (elementBits == 1) return shifted;

    const std::uint32_t scaleConstant =
        addConstant(eir::BitVector::fromUInt64(elementBits, shifted.type.bitWidth()));
    const std::uint32_t scaleRegister = newRegister(shifted.type);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(scaleRegister),
                                eir::Operand::constant(scaleConstant)));

    const std::uint32_t result = newRegister(shifted.type);
    emit(eir::Instruction::make(eir::Opcode::Multiply, eir::Operand::reg(result),
                                eir::Operand::reg(shifted.reg), eir::Operand::reg(scaleRegister)));
    return Value{.reg = result, .type = shifted.type};
}

ProcessTranslator::Value ProcessTranslator::widenForOffsetArithmetic(const Value& index) {
    // Thirty-two bits addresses any object Enzan can hold, because a vector's width is a
    // 32-bit quantity to begin with. Signedness is preserved so that a negative index stays
    // negative: it then becomes a very large unsigned offset, the select reads out of range,
    // and the result is X — which is what IEEE 1800 requires for an out-of-range select.
    const std::uint32_t width = std::max(index.type.bitWidth(), 32U);
    if (index.type.bitWidth() == width) return index;

    return coerce(index, eir::Type::bits(width, index.type.isSigned(), true));
}

ProcessTranslator::Value ProcessTranslator::shiftIndexToBitOffset(const Value& index,
                                                                  std::int32_t rangeLow) {
    // Widened **before** any arithmetic, always, not only when there is a subtraction to do.
    //
    // A bit offset is arithmetic on a bit position, and an index is only as wide as the
    // thing it indexes: a 2-bit pointer into a four-element packed array, multiplied by an
    // element width of 8, wraps to zero in two bits and every element then aliases the
    // first. That defect reads and writes the wrong storage while looking entirely
    // plausible, and it is what the differential test against Icarus Verilog caught.
    const Value widened = widenForOffsetArithmetic(index);
    if (rangeLow == 0) return widened;

    const eir::Type indexType = widened.type;

    const std::uint32_t lowConstant =
        addConstant(eir::BitVector::fromInt64(rangeLow, indexType.bitWidth()));
    const std::uint32_t lowRegister = newRegister(indexType);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(lowRegister),
                                eir::Operand::constant(lowConstant)));

    const std::uint32_t result = newRegister(indexType);
    emit(eir::Instruction::make(eir::Opcode::Subtract, eir::Operand::reg(result),
                                eir::Operand::reg(widened.reg), eir::Operand::reg(lowRegister)));
    return Value{.reg = result, .type = indexType};
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::lowBitOfRangeSelect(const ast::RangeSelectExpression& select,
                                       std::int32_t rangeLow, eir::Type partType,
                                       std::uint32_t elementBits) {
    const eir::Type offsetType = eir::Type::bits(32, false, false);

    const auto constantOffset = [&](std::int64_t offset) -> std::optional<Value> {
        if (offset < 0) {
            module->reportUnsupported(select.sourceRange.start(),
                                      "a part select that starts below the declared range");
            failed = true;
            return std::nullopt;
        }
        const std::uint32_t constant =
            addConstant(eir::BitVector::fromUInt64(static_cast<std::uint64_t>(offset), 32));
        const std::uint32_t result = newRegister(offsetType);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                    eir::Operand::constant(constant)));
        return Value{.reg = result, .type = offsetType};
    };

    switch (select.getSelectionKind()) {
    case ast::RangeSelectionKind::Simple: {
        const std::optional<std::int64_t> left = constantValueOf(select.left());
        const std::optional<std::int64_t> right = constantValueOf(select.right());
        if (!left.has_value() || !right.has_value()) {
            // A simple range select must have constant bounds in the language; reaching
            // here means slang kept them symbolic in a way this frontend cannot fold.
            module->reportUnsupported(select.sourceRange.start(),
                                      "a part select whose bounds are not constant here");
            failed = true;
            return std::nullopt;
        }
        return constantOffset(std::min(*left, *right) - rangeLow);
    }

    case ast::RangeSelectionKind::IndexedUp: {
        // `a[base +: width]` selects upwards from `base`, so `base` is the low bit.
        const std::optional<Value> base = translateExpression(select.left());
        if (!base.has_value()) return std::nullopt;
        return scaleIndexToBitOffset(*base, rangeLow, elementBits);
    }

    case ast::RangeSelectionKind::IndexedDown: {
        // `a[base -: width]` selects downwards, so the low bit is `base - width + 1`.
        const std::optional<Value> base = translateExpression(select.left());
        if (!base.has_value()) return std::nullopt;

        const std::optional<Value> scaled = scaleIndexToBitOffset(*base, rangeLow, elementBits);
        if (!scaled.has_value()) return std::nullopt;
        const Value shifted = *scaled;
        const std::uint32_t adjustment = addConstant(eir::BitVector::fromInt64(
            static_cast<std::int64_t>(partType.bitWidth()) - 1, shifted.type.bitWidth()));
        const std::uint32_t adjustmentRegister = newRegister(shifted.type);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant,
                                    eir::Operand::reg(adjustmentRegister),
                                    eir::Operand::constant(adjustment)));

        const std::uint32_t result = newRegister(shifted.type);
        emit(eir::Instruction::make(eir::Opcode::Subtract, eir::Operand::reg(result),
                                    eir::Operand::reg(shifted.reg),
                                    eir::Operand::reg(adjustmentRegister)));
        return Value{.reg = result, .type = shifted.type};
    }
    }

    module->reportUnsupported(select.sourceRange.start(), "this part select form");
    failed = true;
    return std::nullopt;
}

std::optional<std::uint32_t> ProcessTranslator::objectOfLValue(const ast::Expression& base) {
    if (base.kind != ast::ExpressionKind::NamedValue
        && base.kind != ast::ExpressionKind::HierarchicalValue) {
        module->reportUnsupported(base.sourceRange.start(),
                                  "writing through anything but a plain signal reference");
        failed = true;
        return std::nullopt;
    }

    const std::optional<std::uint32_t> objectIndex =
        module->objectFor(base.as<ast::ValueExpressionBase>().symbol);
    if (!objectIndex.has_value()) failed = true;
    return objectIndex;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateElementSelect(const ast::Expression& expression) {
    const auto& select = expression.as<ast::ElementSelectExpression>();

    const std::optional<Value> base = translateExpression(select.value());
    if (!base.has_value()) return std::nullopt;

    const slang::ConstantRange range = select.value().type->getFixedRange();
    if (range.left < range.right) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  "an ascending declared range such as `[0:N]`");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Value> index = translateExpression(select.selector());
    if (!index.has_value()) return std::nullopt;

    const std::optional<eir::Type> elementType = module->convertType(*expression.type);
    if (!elementType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this element type");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Value> offset =
        scaleIndexToBitOffset(*index, range.right, elementBitsOf(*select.value().type));
    if (!offset.has_value()) return std::nullopt;

    // One bit is a bit select; anything wider is a window, which is what an element of a
    // packed array such as `logic [3:0][7:0] mem` is. Treating every element select as a bit
    // select would silently read one bit of a byte.
    const std::uint32_t result = newRegister(*elementType);
    emit(eir::Instruction::make(
        elementType->bitWidth() == 1 ? eir::Opcode::BitSelect : eir::Opcode::PartSelect,
        eir::Operand::reg(result), eir::Operand::reg(base->reg), eir::Operand::reg(offset->reg)));
    return Value{.reg = result, .type = *elementType};
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateRangeSelect(const ast::Expression& expression) {
    const auto& select = expression.as<ast::RangeSelectExpression>();

    const std::optional<Value> base = translateExpression(select.value());
    if (!base.has_value()) return std::nullopt;

    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this part select type");
        failed = true;
        return std::nullopt;
    }

    const slang::ConstantRange range = select.value().type->getFixedRange();
    if (range.left < range.right) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  "an ascending declared range such as `[0:N]`");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Value> low =
        lowBitOfRangeSelect(select, range.right, *resultType, elementBitsOf(*select.value().type));
    if (!low.has_value()) return std::nullopt;

    const std::uint32_t result = newRegister(*resultType);
    emit(eir::Instruction::make(eir::Opcode::PartSelect, eir::Operand::reg(result),
                                eir::Operand::reg(base->reg), eir::Operand::reg(low->reg)));
    return Value{.reg = result, .type = *resultType};
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateCall(const ast::Expression& expression) {
    const auto& call = expression.as<ast::CallExpression>();

    // Anything slang folded to a constant — `$bits`, `$clog2`, a constant function call —
    // is already an answer and needs no runtime support at all.
    if (const slang::ConstantValue* constant = expression.getConstant();
        constant != nullptr && constant->isInteger()) {
        const std::optional<eir::Type> type = module->convertType(*expression.type);
        if (type.has_value()) {
            const std::uint32_t index = addConstant(toBitVector(constant->integer()));
            const std::uint32_t result = newRegister(*type);
            emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                        eir::Operand::constant(index)));
            return Value{.reg = result, .type = *type};
        }
    }

    if (call.isSystemCall()) {
        const std::string_view name = call.getSubroutineName();
        if (name == "$time" || name == "$stime" || name == "$realtime") {
            const std::optional<eir::Type> type = module->convertType(*expression.type);
            const eir::Type resultType = type.value_or(eir::Type::bits(64, false, false));
            const std::uint32_t result = newRegister(resultType);
            emit(eir::Instruction::make(eir::Opcode::ReadTime, eir::Operand::reg(result)));
            return Value{.reg = result, .type = resultType};
        }

        module->reportUnsupported(expression.sourceRange.start(),
                                  std::string("the system function ") + std::string(name));
        failed = true;
        return std::nullopt;
    }

    module->reportUnsupported(expression.sourceRange.start(), "user-defined tasks and functions");
    failed = true;
    return std::nullopt;
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateAssignment(const ast::Expression& expression) {
    const auto& assignment = expression.as<ast::AssignmentExpression>();

    if (assignment.timingControl != nullptr) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  "a timing control on an assignment, such as `a <= #5 b`");
        failed = true;
        return std::nullopt;
    }

    // A compound assignment is already rewritten by slang: `a += b` becomes
    // `a = <lvalue> + b`, where `<lvalue>` is an LValueReference standing for the left-hand
    // side's *current* value. The reference exists so that the target is evaluated exactly
    // once, so it is read once here and bound for the right-hand side to pick up. Applying
    // `assignment.op` again on top of that would double the operation.
    std::optional<Value> savedReference;
    if (assignment.isCompound()) {
        const std::optional<Value> existing = translateExpression(assignment.left());
        if (!existing.has_value()) return std::nullopt;

        savedReference = lvalueReference;
        lvalueReference = existing;
    }

    const std::optional<Value> value = translateExpression(assignment.right());

    if (assignment.isCompound()) lvalueReference = savedReference;

    if (!value.has_value()) return std::nullopt;

    if (!assignTo(assignment.left(), *value, assignment.isNonBlocking())) return std::nullopt;

    return value;
}

bool ProcessTranslator::assignTo(const ast::Expression& target, const Value& value,
                                 bool nonBlocking) {
    switch (target.kind) {
    case ast::ExpressionKind::NamedValue:
    case ast::ExpressionKind::HierarchicalValue: {
        const std::optional<std::uint32_t> objectIndex =
            module->objectFor(target.as<ast::ValueExpressionBase>().symbol);
        if (!objectIndex.has_value()) {
            failed = true;
            return false;
        }

        const eir::Type objectType = module->definition().objects[*objectIndex].type;
        const Value widened = coerce(value, objectType);
        const Value stored = coerceState(widened, objectType);

        noteWrite(*objectIndex);
        emit(eir::Instruction::make(
            nonBlocking ? eir::Opcode::StoreNonBlocking : eir::Opcode::Store, {},
            eir::Operand::object(*objectIndex), eir::Operand::reg(stored.reg)));
        return true;
    }

    case ast::ExpressionKind::ElementSelect: {
        const auto& select = target.as<ast::ElementSelectExpression>();
        const std::optional<std::uint32_t> objectIndex = objectOfLValue(select.value());
        if (!objectIndex.has_value()) return false;

        const slang::ConstantRange range = select.value().type->getFixedRange();
        if (range.left < range.right) {
            module->reportUnsupported(target.sourceRange.start(),
                                      "an ascending declared range such as `[0:N]`");
            failed = true;
            return false;
        }

        const std::optional<Value> index = translateExpression(select.selector());
        if (!index.has_value()) return false;

        const std::optional<eir::Type> elementType = module->convertType(*target.type);
        if (!elementType.has_value()) {
            module->reportUnsupported(target.sourceRange.start(), "this element type");
            failed = true;
            return false;
        }

        const std::optional<Value> offset =
            scaleIndexToBitOffset(*index, range.right, elementBitsOf(*select.value().type));
        if (!offset.has_value()) return false;

        const Value element = coerce(value, *elementType);
        noteWrite(*objectIndex);
        emit(eir::Instruction::make(
            nonBlocking ? eir::Opcode::StorePartNonBlocking : eir::Opcode::StorePart, {},
            eir::Operand::object(*objectIndex), eir::Operand::reg(element.reg),
            eir::Operand::reg(offset->reg)));
        return true;
    }

    case ast::ExpressionKind::RangeSelect: {
        const auto& select = target.as<ast::RangeSelectExpression>();
        const std::optional<std::uint32_t> objectIndex = objectOfLValue(select.value());
        if (!objectIndex.has_value()) return false;

        const slang::ConstantRange range = select.value().type->getFixedRange();
        if (range.left < range.right) {
            module->reportUnsupported(target.sourceRange.start(),
                                      "an ascending declared range such as `[0:N]`");
            failed = true;
            return false;
        }

        const std::optional<eir::Type> partType = module->convertType(*target.type);
        if (!partType.has_value()) {
            module->reportUnsupported(target.sourceRange.start(), "this part select type");
            failed = true;
            return false;
        }

        const std::optional<Value> low = lowBitOfRangeSelect(select, range.right, *partType,
                                                             elementBitsOf(*select.value().type));
        if (!low.has_value()) return false;

        const Value part = coerce(value, *partType);
        noteWrite(*objectIndex);
        emit(eir::Instruction::make(nonBlocking ? eir::Opcode::StorePartNonBlocking
                                                : eir::Opcode::StorePart,
                                    {}, eir::Operand::object(*objectIndex),
                                    eir::Operand::reg(part.reg), eir::Operand::reg(low->reg)));
        return true;
    }

    case ast::ExpressionKind::Concatenation: {
        // `{a, b} = value` splits the value across the operands, most significant
        // first. Each piece is extracted and then assigned by the same rules, so a
        // concatenation of part selects works without a special case.
        const auto& concatenation = target.as<ast::ConcatenationExpression>();
        std::uint32_t offset = value.type.bitWidth();

        for (const ast::Expression* operand : concatenation.operands()) {
            const std::optional<eir::Type> pieceType = module->convertType(*operand->type);
            if (!pieceType.has_value()) {
                module->reportUnsupported(operand->sourceRange.start(),
                                          "this concatenation target type");
                failed = true;
                return false;
            }
            if (pieceType->bitWidth() > offset) {
                module->reportUnsupported(target.sourceRange.start(),
                                          "a concatenation target wider than its value");
                failed = true;
                return false;
            }
            offset -= pieceType->bitWidth();

            const std::uint32_t offsetConstant =
                addConstant(eir::BitVector::fromUInt64(offset, 32));
            const std::uint32_t offsetRegister = newRegister(eir::Type::bits(32, false, false));
            emit(eir::Instruction::make(eir::Opcode::LoadConstant,
                                        eir::Operand::reg(offsetRegister),
                                        eir::Operand::constant(offsetConstant)));

            const std::uint32_t piece = newRegister(*pieceType);
            emit(eir::Instruction::make(eir::Opcode::PartSelect, eir::Operand::reg(piece),
                                        eir::Operand::reg(value.reg),
                                        eir::Operand::reg(offsetRegister)));

            if (!assignTo(*operand, Value{.reg = piece, .type = *pieceType}, nonBlocking))
                return false;
        }
        return true;
    }

    default: break;
    }

    module->reportUnsupported(target.sourceRange.start(),
                              std::string(toString(target.kind))
                                  + " expressions as an assignment target");
    failed = true;
    return false;
}

bool ProcessTranslator::translateSystemTask(const ast::Expression& expressionNode) {
    const auto& call = expressionNode.as<ast::CallExpression>();
    const std::string_view name = call.getSubroutineName();

    eir::SysTaskKind kind = eir::SysTaskKind::Display;
    bool skipFirstArgument = false;
    bool ignoreArguments = false;

    if (name == "$display" || name == "$displayb" || name == "$displayo" || name == "$displayh") {
        kind = eir::SysTaskKind::Display;
    } else if (name == "$write" || name == "$writeb" || name == "$writeo" || name == "$writeh") {
        kind = eir::SysTaskKind::Write;
    } else if (name == "$finish" || name == "$stop") {
        // `$stop` would return control to an interactive prompt, which needs the control
        // layer of M3. Treating it as `$finish` ends the run instead of ignoring it, which
        // is the safer of the two wrong answers.
        kind = eir::SysTaskKind::Finish;
        ignoreArguments = true;
    } else if (name == "$fatal") {
        kind = eir::SysTaskKind::Fatal;
        // `$fatal`'s first argument is the finish code, not part of the message.
        skipFirstArgument = !call.arguments().empty()
                            && call.arguments()[0]->kind != ast::ExpressionKind::StringLiteral;
    } else if (name == "$error") {
        kind = eir::SysTaskKind::Error;
    } else if (name == "$warning") {
        kind = eir::SysTaskKind::Warning;
    } else if (name == "$info") {
        kind = eir::SysTaskKind::Info;
    } else if (name == "$dumpfile") {
        kind = eir::SysTaskKind::DumpFile;
    } else if (name == "$dumpvars") {
        kind = eir::SysTaskKind::DumpVars;
        ignoreArguments = true;
    } else if (name == "$dumpon" || name == "$dumpoff" || name == "$dumpall" || name == "$dumplimit"
               || name == "$dumpflush") {
        // Accepted and ignored: they control a dump this writer does not pause or split,
        // and failing a design over them would be worse than doing nothing.
        return true;
    } else {
        module->reportUnsupported(expressionNode.sourceRange.start(),
                                  std::string("the system task ") + std::string(name));
        failed = true;
        return false;
    }

    eir::SysTaskCall descriptor;
    descriptor.kind = kind;
    descriptor.location = module->convertLocation(expressionNode.sourceRange.start());

    std::span<const ast::Expression* const> arguments = call.arguments();
    if (skipFirstArgument && !arguments.empty()) arguments = arguments.subspan(1);

    if (!ignoreArguments) {
        bool first = true;
        for (const ast::Expression* argument : arguments) {
            if (first && argument->kind == ast::ExpressionKind::StringLiteral) {
                descriptor.formatIndex =
                    addString(std::string(argument->as<ast::StringLiteral>().getValue()));
                first = false;
                continue;
            }
            first = false;

            const std::optional<Value> value = translateExpression(*argument);
            if (!value.has_value()) return false;

            descriptor.arguments.push_back(
                eir::SysTaskArgument{.value = eir::Operand::reg(value->reg), .type = value->type});
        }
    } else if (!arguments.empty() && arguments[0]->kind == ast::ExpressionKind::StringLiteral) {
        descriptor.formatIndex =
            addString(std::string(arguments[0]->as<ast::StringLiteral>().getValue()));
    }

    const auto index = static_cast<std::uint32_t>(process.sysCalls.size());
    process.sysCalls.push_back(std::move(descriptor));
    emit(eir::Instruction::make(eir::Opcode::SysTask, {}, eir::Operand::sysCall(index)));
    return true;
}

std::optional<std::int64_t> ProcessTranslator::constantValueOf(const ast::Expression& expression) {
    if (expression.kind == ast::ExpressionKind::Conversion)
        return constantValueOf(expression.as<ast::ConversionExpression>().operand());

    if (expression.kind == ast::ExpressionKind::IntegerLiteral)
        return expression.as<ast::IntegerLiteral>().getValue().as<std::int64_t>();

    if (expression.kind == ast::ExpressionKind::NamedValue) {
        const ast::ValueSymbol& symbol = expression.as<ast::ValueExpressionBase>().symbol;
        if (symbol.kind == slang::ast::SymbolKind::Parameter) {
            const slang::ConstantValue& value = symbol.as<ast::ParameterSymbol>().getValue();
            if (value.isInteger()) return value.integer().as<std::int64_t>();
        }
    }

    if (const slang::ConstantValue* constant = expression.getConstant();
        constant != nullptr && constant->isInteger()) {
        return constant->integer().as<std::int64_t>();
    }

    return std::nullopt;
}

} // namespace enzan::frontend::sv
