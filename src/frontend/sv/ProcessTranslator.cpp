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
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/Type.h"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <format>
#include <map>
#include <tuple>
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
    // Every instruction is stamped with the statement being translated, which is the whole of
    // what source stepping and a line breakpoint need (DESIGN_GUIDE.md section 10.4). Doing it
    // here rather than at each of the hundred call sites is the only way it stays true: a
    // single emission that forgot would be a line the debugger can never stop on, and nothing
    // about the simulation would look wrong.
    eir::Instruction stamped = instruction;
    stamped.location = currentStatement;
    process.blocks[current].instructions.push_back(stamped);
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

bool ProcessTranslator::noteWrite(std::uint32_t objectIndex, std::int64_t lowBit,
                                  std::int64_t width) {
    const std::int64_t objectWidth = module->definition().objects[objectIndex].type.bitWidth();
    const std::int64_t low = std::max<std::int64_t>(lowBit, 0);
    const std::int64_t end = std::min(lowBit + width, objectWidth);
    if (end <= low) return false;

    writes.push_back(eir::WriteExtent{.objectIndex = objectIndex,
                                      .lowBit = static_cast<std::uint32_t>(low),
                                      .width = static_cast<std::uint32_t>(end - low)});
    return true;
}

void ProcessTranslator::noteWholeWrite(std::uint32_t objectIndex) {
    writes.push_back(eir::WriteExtent::whole(
        objectIndex, module->definition().objects[objectIndex].type.bitWidth()));
}

std::pair<std::int64_t, std::int64_t>
ProcessTranslator::reachOfSelect(const ast::Expression& index, std::int32_t rangeLow,
                                 std::uint32_t elementBits, std::uint32_t width, bool downward,
                                 std::uint32_t objectWidth) {
    const std::uint32_t indexBits = index.type->getBitWidth();
    if (indexBits == 0 || indexBits >= 32) return {0, objectWidth};

    const bool isSigned = index.type->isSigned();
    const std::int64_t lowest = isSigned ? -(std::int64_t{1} << (indexBits - 1)) : 0;
    const std::int64_t highest =
        isSigned ? (std::int64_t{1} << (indexBits - 1)) - 1 : (std::int64_t{1} << indexBits) - 1;

    // Element positions are clamped to one past either end of the object before they are
    // scaled, which keeps the product inside 64 bits for any element width and changes
    // nothing that survives the clamp to the object afterwards.
    const std::int64_t elements = objectWidth / std::max<std::uint32_t>(elementBits, 1);
    const auto bitOf = [&](std::int64_t position) {
        const std::int64_t clamped =
            std::clamp<std::int64_t>(position - rangeLow, -elements - 1, elements + 1);
        const std::int64_t bit = clamped * elementBits;
        return downward ? bit - static_cast<std::int64_t>(width) + 1 : bit;
    };

    const std::int64_t low = bitOf(lowest);
    return {low, bitOf(highest) + width - low};
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
    eir::normaliseWriteSet(writes);

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

    noteWholeWrite(objectIndex);
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

    // Set on the way in and left, rather than saved and restored. A statement's own
    // instructions are emitted between here and the next statement, and an enclosing
    // construct that emits again afterwards — a loop's back edge, a conditional's join — is
    // better attributed to the last statement that ran than to the line of the `for` above
    // it, which is where a reader would not expect to stop.
    const SourceLocation location = module->convertLocation(statement.sourceRange.start());
    if (location.valid()) currentStatement = location;

    switch (statement.kind) {
    case ast::StatementKind::Empty: return true;

    case ast::StatementKind::VariableDeclaration: {
        // A declaration's storage is created on demand when something references it, and an
        // initialiser on a procedural variable is its time-zero value. A local of an inlined
        // subroutine is automatic instead, and its initialiser runs where it is declared, on
        // every call.
        const ast::VariableSymbol& variable = statement.as<ast::VariableDeclStatement>().symbol;
        const Value* local = localFor(variable);
        const ast::Expression* initializer = variable.getInitializer();
        if (local == nullptr || initializer == nullptr) return true;

        const Value target = *local;
        const std::optional<Value> value = translateExpression(*initializer);
        if (!value.has_value()) return false;
        const Value stored = coerceState(coerce(*value, target.type), target.type);
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(target.reg),
                                    eir::Operand::reg(stored.reg)));
        return true;
    }

    case ast::StatementKind::Return: return translateReturn(statement);

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

bool ProcessTranslator::translateReturn(const ast::Statement& statement) {
    if (inlineFrames.empty()) {
        module->reportUnsupported(statement.sourceRange.start(),
                                  "a `return` outside a function or task");
        failed = true;
        return false;
    }
    const InlineFrame& frame = inlineFrames.back();
    const ast::Expression* expression = statement.as<ast::ReturnStatement>().expr;

    if (expression != nullptr && frame.returnVariable != nullptr) {
        const Value* returned = localFor(*frame.returnVariable);
        if (returned == nullptr) {
            failed = true;
            return false;
        }
        const Value target = *returned;
        const std::optional<Value> value = translateExpression(*expression);
        if (!value.has_value()) return false;
        const Value stored = coerceState(coerce(*value, target.type), target.type);
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(target.reg),
                                    eir::Operand::reg(stored.reg)));
    }

    emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(frame.exitBlock)));

    // Anything after a `return` is unreachable but still has to be emitted somewhere.
    selectBlock(newBlock("after-return"));
    return true;
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

    // How an item is compared with the selector (IEEE 1800 section 12.5): exactly for `case`,
    // with Z, or X and Z, on either side as a don't-care for `casez` and `casex`, and as
    // `inside` compares for `case ... inside`.
    eir::Opcode matchOpcode = eir::Opcode::CaseEqual;
    switch (caseStatement.condition) {
    case ast::CaseStatementCondition::Normal: matchOpcode = eir::Opcode::CaseEqual; break;
    case ast::CaseStatementCondition::WildcardJustZ: matchOpcode = eir::Opcode::CaseZMatch; break;
    case ast::CaseStatementCondition::WildcardXOrZ: matchOpcode = eir::Opcode::CaseXMatch; break;
    case ast::CaseStatementCondition::Inside: break;
    }
    const bool inside = caseStatement.condition == ast::CaseStatementCondition::Inside;

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
            // An item of a plain `case` matches by `===`, not `==`: `2'bx1` matches a selector
            // of `2'bx1`, and an unknown selector matches no item that differs from it. An X
            // from `case ... inside` takes the branch no more than an `if` would.
            std::optional<Value> matchedValue;
            if (inside) {
                matchedValue = insideItem(*selector, *item);
            } else {
                const std::optional<Value> itemValue = translateExpression(*item);
                if (itemValue.has_value())
                    matchedValue = compare(matchOpcode, *selector, *itemValue);
            }
            if (!matchedValue.has_value()) return false;
            const std::uint32_t matched = matchedValue->reg;

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

    // A variable declared in the header, `for (int i = 0; ...)`, is automatic and starts again
    // every time the loop does. Its initialiser is therefore an assignment at the loop's entry,
    // not only a value at time zero: without one an inner loop runs once, on the first pass of
    // the loop around it, and finds its variable already at its limit on every later pass.
    for (const ast::VariableSymbol* variable : loop.loopVars) {
        const ast::Expression* initializer = variable->getInitializer();
        if (initializer == nullptr) continue;

        // Inside an inlined subroutine the variable is a local, bound to a register.
        if (const Value* local = localFor(*variable)) {
            // Copied first: a call in the initialiser rebinds the locals on its way out.
            const Value target = *local;
            const std::optional<Value> value = translateExpression(*initializer);
            if (!value.has_value()) return false;
            const Value stored = coerceState(coerce(*value, target.type), target.type);
            emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(target.reg),
                                        eir::Operand::reg(stored.reg)));
            continue;
        }

        const std::optional<std::uint32_t> objectIndex = module->objectFor(*variable);
        if (!objectIndex.has_value()) {
            failed = true;
            return false;
        }
        const std::optional<Value> value = translateExpression(*initializer);
        if (!value.has_value()) return false;

        const eir::Type objectType = module->definition().objects[*objectIndex].type;
        const Value stored = coerceState(coerce(*value, objectType), objectType);
        noteWholeWrite(*objectIndex);
        emit(eir::Instruction::make(eir::Opcode::Store, {}, eir::Operand::object(*objectIndex),
                                    eir::Operand::reg(stored.reg)));
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
    if (expression.kind == ast::ExpressionKind::Call
        && !expression.as<ast::CallExpression>().isSystemCall()) {
        return inlineCall(expression.as<ast::CallExpression>(), expression).has_value();
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

        // A local of a subroutine being inlined lives in a register.
        if (const Value* local = localFor(symbol)) return copyOf(*local);

        // A parameter or an enumeration's value is a constant, not state: reading it must not
        // create an object, because an object would be a per-instance slot for a value that
        // never changes.
        if (symbol.kind == slang::ast::SymbolKind::Parameter
            || symbol.kind == slang::ast::SymbolKind::EnumValue) {
            const slang::ConstantValue& value = symbol.kind == slang::ast::SymbolKind::Parameter
                                                    ? symbol.as<ast::ParameterSymbol>().getValue()
                                                    : symbol.as<ast::EnumValueSymbol>().getValue();
            if (!type.has_value()) break;
            const std::optional<eir::BitVector> bits =
                module->constantBits(value, *expression.type);
            if (!bits.has_value()) break;
            const std::uint32_t constant = addConstant(*bits);
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

    case ast::ExpressionKind::ElementSelect:
    case ast::ExpressionKind::RangeSelect:
    case ast::ExpressionKind::MemberAccess: {
        const std::optional<Location> location = locate(expression);
        if (!location.has_value()) return std::nullopt;
        return readLocation(*location);
    }

    case ast::ExpressionKind::Call: return translateCall(expression);

    case ast::ExpressionKind::Inside: return translateInside(expression);

    case ast::ExpressionKind::SimpleAssignmentPattern:
    case ast::ExpressionKind::StructuredAssignmentPattern:
    case ast::ExpressionKind::ReplicatedAssignmentPattern:
        return translateAssignmentPattern(expression);

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

ProcessTranslator::Value ProcessTranslator::compare(eir::Opcode opcode, const Value& left,
                                                    const Value& right) {
    // Comparison operands keep their own width, extended to whichever is wider, because the
    // result is one bit and the comparison itself decides the signedness.
    const bool isSigned = left.type.isSigned() && right.type.isSigned();
    const std::uint32_t width = std::max(left.type.bitWidth(), right.type.bitWidth());
    const eir::Type operandType = eir::Type::bits(width, isSigned, true);

    const Value a = coerce(left, operandType);
    const Value b = coerce(right, operandType);
    const std::uint32_t result = newRegister(eir::Type::logic());
    eir::Instruction instruction = eir::Instruction::make(
        opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg), eir::Operand::reg(b.reg));
    instruction.isSigned = isSigned;
    emit(instruction);
    return Value{.reg = result, .type = eir::Type::logic()};
}

std::optional<ProcessTranslator::Value> ProcessTranslator::insideItem(const Value& value,
                                                                      const ast::Expression& item) {
    if (item.kind == ast::ExpressionKind::ValueRange) {
        const auto& range = item.as<ast::ValueRangeExpression>();
        if (range.rangeKind != ast::ValueRangeKind::Simple) {
            module->reportUnsupported(item.sourceRange.start(), "a tolerance range");
            failed = true;
            return std::nullopt;
        }
        const std::optional<Value> low = translateExpression(range.left());
        if (!low.has_value()) return std::nullopt;
        const std::optional<Value> high = translateExpression(range.right());
        if (!high.has_value()) return std::nullopt;

        // `low <= value && value <= high`, four-state, so that an unknown bound or value
        // gives X rather than a guess.
        const Value aboveLow = compare(eir::Opcode::LessEqual, *low, value);
        const Value belowHigh = compare(eir::Opcode::LessEqual, value, *high);
        const std::uint32_t result = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(result),
                                    eir::Operand::reg(aboveLow.reg),
                                    eir::Operand::reg(belowHigh.reg)));
        return Value{.reg = result, .type = eir::Type::logic()};
    }

    const std::optional<Value> itemValue = translateExpression(item);
    if (!itemValue.has_value()) return std::nullopt;
    return compare(eir::Opcode::WildcardEqual, value, *itemValue);
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateInside(const ast::Expression& expression) {
    const auto& inside = expression.as<ast::InsideExpression>();

    const std::optional<Value> value = translateExpression(inside.left());
    if (!value.has_value()) return std::nullopt;

    // The items are combined with a four-state OR: 1 if any matches, otherwise X if any could,
    // otherwise 0, which is the result IEEE 1800 section 11.4.13 defines.
    std::optional<Value> result;
    for (const ast::Expression* item : inside.rangeList()) {
        const std::optional<Value> matched = insideItem(*value, *item);
        if (!matched.has_value()) return std::nullopt;
        if (!result.has_value()) {
            result = matched;
            continue;
        }
        const std::uint32_t combined = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::Or, eir::Operand::reg(combined),
                                    eir::Operand::reg(result->reg),
                                    eir::Operand::reg(matched->reg)));
        result = Value{.reg = combined, .type = eir::Type::logic()};
    }
    return result;
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

    const auto comparison = [&](eir::Opcode opcode, bool swapOperands) -> std::optional<Value> {
        return compare(opcode, swapOperands ? *right : *left, swapOperands ? *left : *right);
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
    case ast::BinaryOperator::WildcardEquality:
        return comparison(eir::Opcode::WildcardEqual, false);
    case ast::BinaryOperator::WildcardInequality:
        return comparison(eir::Opcode::WildcardNotEqual, false);
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

    case ast::BinaryOperator::Power: {
        // The exponent is self-determined, as a shift amount is, and its signedness decides
        // what a negative one means, so it travels as an immediate.
        const Value base = coerce(*left, *resultType);
        const std::uint32_t result = newRegister(*resultType);
        eir::Instruction instruction =
            eir::Instruction::make(eir::Opcode::Power, eir::Operand::reg(result),
                                   eir::Operand::reg(base.reg), eir::Operand::reg(right->reg),
                                   eir::Operand::immediate(right->type.isSigned() ? 1U : 0U));
        instruction.isSigned = resultType->isSigned();
        emit(instruction);
        return Value{.reg = result, .type = *resultType};
    }
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
ProcessTranslator::translateAssignmentPattern(const ast::Expression& expression) {
    const std::optional<eir::Type> resultType = module->convertType(*expression.type);
    if (!resultType.has_value()) {
        module->reportUnsupported(expression.sourceRange.start(), "this assignment pattern type");
        failed = true;
        return std::nullopt;
    }

    // slang expands every form — positional, by member, by default, replicated — into the
    // elements in order, each converted to the type it initialises. In order, the first one
    // highest, is how the storage lays out an array or a struct, so the pattern is the
    // concatenation of its elements.
    const ast::AssignmentPatternExpressionBase* pattern = nullptr;
    switch (expression.kind) {
    case ast::ExpressionKind::SimpleAssignmentPattern:
        pattern = &expression.as<ast::SimpleAssignmentPatternExpression>();
        break;
    case ast::ExpressionKind::StructuredAssignmentPattern:
        pattern = &expression.as<ast::StructuredAssignmentPatternExpression>();
        break;
    default: pattern = &expression.as<ast::ReplicatedAssignmentPatternExpression>(); break;
    }
    const std::uint32_t zero =
        addConstant(eir::BitVector::filled(resultType->bitWidth(), eir::Logic::Zero));
    const std::uint32_t result = newRegister(*resultType);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                eir::Operand::constant(zero)));

    std::uint32_t offset = resultType->bitWidth();
    for (const ast::Expression* element : pattern->elements()) {
        const std::optional<eir::Type> elementType = module->convertType(*element->type);
        if (!elementType.has_value()) {
            module->reportUnsupported(element->sourceRange.start(), "this element type");
            failed = true;
            return std::nullopt;
        }
        const std::optional<Value> value = translateExpression(*element);
        if (!value.has_value()) return std::nullopt;
        const Value stored = coerceState(coerce(*value, *elementType), *elementType);

        if (elementType->bitWidth() > offset) {
            module->reportUnsupported(expression.sourceRange.start(),
                                      "an assignment pattern whose elements exceed its width");
            failed = true;
            return std::nullopt;
        }
        offset -= elementType->bitWidth();

        const Value at = offsetConstant(offset);
        emit(eir::Instruction::make(eir::Opcode::SplicePart, eir::Operand::reg(result),
                                    eir::Operand::reg(stored.reg), eir::Operand::reg(at.reg)));
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

ProcessTranslator::Value ProcessTranslator::widenForOffsetArithmetic(const Value& index) {
    // Thirty-two bits addresses any object Enzan can hold, because a vector's width is a
    // 32-bit quantity to begin with. Signedness is preserved so that a negative index stays
    // negative: it then becomes a very large unsigned offset, the select reads out of range,
    // and the result is X — which is what IEEE 1800 requires for an out-of-range select.
    const std::uint32_t width = std::max(index.type.bitWidth(), 32U);
    if (index.type.bitWidth() == width) return index;

    return coerce(index, eir::Type::bits(width, index.type.isSigned(), true));
}

// ---------------------------------------------------------------------------
// Locations: selects and member accesses
// ---------------------------------------------------------------------------
//
// Every select and member access is a window of bits: an element of an array, a field of a
// struct, a bit or a part of a vector. An unpacked aggregate is stored flat, left element and
// first field highest, as a packed one is (DESIGN_GUIDE.md section 4.3), so one rule places a
// window whatever it is a window of, and a chain of them — `mem[i].field[3:0]` — composes into
// one offset. A window in an object is read in place and written with a part store; a window
// in a value that is not an object, a parameter for instance, is selected from a register.

namespace {

/// The position from the least significant end, in elements, of `index` in a dimension
/// declared `range`: the right index is at position 0, whichever way the range runs.
std::int64_t positionOf(std::int64_t index, slang::ConstantRange range) {
    return range.left >= range.right ? index - range.right : range.right - index;
}

bool inRange(std::int64_t index, slang::ConstantRange range) {
    return index >= range.lower() && index <= range.upper();
}

} // namespace

ProcessTranslator::Value ProcessTranslator::offsetConstant(std::int64_t offset) {
    const eir::Type type = eir::Type::bits(64, true, false);
    const std::uint32_t constant = addConstant(eir::BitVector::fromInt64(offset, 64));
    const std::uint32_t result = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                eir::Operand::constant(constant)));
    return Value{.reg = result, .type = type};
}

ProcessTranslator::Value ProcessTranslator::lowOf(const Location& location) {
    if (location.low.has_value()) return *location.low;
    return offsetConstant(location.constantLow.value_or(0));
}

ProcessTranslator::Value ProcessTranslator::offsetArithmetic(eir::Opcode opcode, const Value& left,
                                                             const Value& right) {
    const std::uint32_t width = std::max(left.type.bitWidth(), right.type.bitWidth());
    const eir::Type type =
        eir::Type::bits(width, left.type.isSigned() && right.type.isSigned(), true);
    const Value a = coerce(left, type);
    const Value b = coerce(right, type);
    const std::uint32_t result = newRegister(type);
    eir::Instruction instruction = eir::Instruction::make(
        opcode, eir::Operand::reg(result), eir::Operand::reg(a.reg), eir::Operand::reg(b.reg));
    instruction.isSigned = type.isSigned();
    emit(instruction);
    return Value{.reg = result, .type = type};
}

ProcessTranslator::Value ProcessTranslator::positionValue(const Value& index,
                                                          slang::ConstantRange range,
                                                          std::uint32_t stride) {
    // Widened before any arithmetic: a 2-bit index into four bytes, scaled in two bits, wraps
    // to zero and every element aliases the first, which looks entirely plausible.
    const Value widened = widenForOffsetArithmetic(index);
    Value position = widened;
    if (range.left >= range.right) {
        if (range.right != 0)
            position =
                offsetArithmetic(eir::Opcode::Subtract, widened, offsetConstant(range.right));
    } else {
        position = offsetArithmetic(eir::Opcode::Subtract, offsetConstant(range.right), widened);
    }
    if (stride == 1) return position;
    return offsetArithmetic(eir::Opcode::Multiply, position, offsetConstant(stride));
}

ProcessTranslator::Value ProcessTranslator::checkedOffset(const Value& offset, const Value& index,
                                                          slang::ConstantRange range) {
    // `lower <= index && index <= upper`, compared as signed 64-bit numbers so that a declared
    // range below zero compares correctly whatever the index's own signedness.
    const Value wide =
        coerce(widenForOffsetArithmetic(index), eir::Type::bits(64, index.type.isSigned(), true));
    const Value signedIndex{.reg = wide.reg, .type = eir::Type::bits(64, true, true)};
    const Value aboveLower =
        compare(eir::Opcode::LessEqual, offsetConstant(range.lower()), signedIndex);
    const Value belowUpper =
        compare(eir::Opcode::LessEqual, signedIndex, offsetConstant(range.upper()));
    const std::uint32_t inside = newRegister(eir::Type::logic());
    emit(eir::Instruction::make(eir::Opcode::And, eir::Operand::reg(inside),
                                eir::Operand::reg(aboveLower.reg),
                                eir::Operand::reg(belowUpper.reg)));

    // Outside the dimension, the offset becomes X, which a read turns into X and a store into
    // nothing; without the check an index past an inner dimension would land in the next
    // element.
    const std::uint32_t unknown = addConstant(eir::BitVector(offset.type.bitWidth()));
    const std::uint32_t unknownRegister = newRegister(offset.type);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(unknownRegister),
                                eir::Operand::constant(unknown)));
    const std::uint32_t result = newRegister(offset.type);
    emit(eir::Instruction::make(eir::Opcode::Conditional, eir::Operand::reg(result),
                                eir::Operand::reg(inside), eir::Operand::reg(offset.reg),
                                eir::Operand::reg(unknownRegister)));
    return Value{.reg = result, .type = offset.type};
}

ProcessTranslator::Location ProcessTranslator::narrowed(const Location& base,
                                                        std::optional<std::int64_t> constantLow,
                                                        std::optional<Value> low, eir::Type type) {
    Location result = base;
    result.type = type;
    result.whole = false;
    if (base.outOfRange) return result;

    if (base.constantLow.has_value() && constantLow.has_value()) {
        result.constantLow = *base.constantLow + *constantLow;
        result.low.reset();
        result.reachLow = *result.constantLow;
        result.reachWidth = type.bitWidth();
        return result;
    }

    const Value baseLow = lowOf(base);
    const Value localLow = low.has_value() ? *low : offsetConstant(constantLow.value_or(0));
    result.constantLow.reset();
    result.low = offsetArithmetic(eir::Opcode::Add, baseLow, localLow);
    // A window that moves is still inside the window it was taken from, which is therefore
    // everything a write through it could reach.
    return result;
}

std::optional<ProcessTranslator::Location>
ProcessTranslator::locate(const ast::Expression& expression) {
    switch (expression.kind) {
    case ast::ExpressionKind::NamedValue:
    case ast::ExpressionKind::HierarchicalValue: {
        const ast::ValueSymbol& symbol = expression.as<ast::ValueExpressionBase>().symbol;
        if (const Value* local = localFor(symbol)) {
            return Location{.root = *local,
                            .type = local->type,
                            .constantLow = 0,
                            .reachWidth = local->type.bitWidth(),
                            .rootIsLocal = true};
        }
        if (symbol.kind == slang::ast::SymbolKind::Parameter
            || symbol.kind == slang::ast::SymbolKind::EnumValue)
            break;

        const std::optional<std::uint32_t> objectIndex = module->objectFor(symbol);
        if (!objectIndex.has_value()) {
            failed = true;
            return std::nullopt;
        }
        const eir::Type type = module->definition().objects[*objectIndex].type;
        return Location{.objectIndex = *objectIndex,
                        .type = type,
                        .constantLow = 0,
                        .reachWidth = type.bitWidth()};
    }

    case ast::ExpressionKind::ElementSelect:
        return locateElement(expression.as<ast::ElementSelectExpression>());

    case ast::ExpressionKind::RangeSelect:
        return locateRange(expression.as<ast::RangeSelectExpression>());

    case ast::ExpressionKind::MemberAccess:
        return locateMember(expression.as<ast::MemberAccessExpression>());

    default: break;
    }

    // Any other value — a parameter, a call, a concatenation — is a window of a register.
    const std::optional<Value> value = translateExpression(expression);
    if (!value.has_value()) return std::nullopt;
    return Location{.root = *value,
                    .type = value->type,
                    .constantLow = 0,
                    .reachWidth = value->type.bitWidth()};
}

std::optional<ProcessTranslator::Location>
ProcessTranslator::locateElement(const ast::ElementSelectExpression& select) {
    const std::optional<Location> base = locate(select.value());
    if (!base.has_value()) return std::nullopt;

    const std::optional<eir::Type> elementType = module->convertType(*select.type);
    if (!elementType.has_value()) {
        module->reportUnsupported(select.sourceRange.start(), "this element type");
        failed = true;
        return std::nullopt;
    }

    const ast::Type& baseType = *select.value().type;
    const slang::ConstantRange range = baseType.getFixedRange();
    const std::uint32_t stride =
        baseType.isUnpackedArray() ? elementType->bitWidth() : elementBitsOf(baseType);

    if (const std::optional<std::int64_t> index = constantValueOf(select.selector())) {
        if (!inRange(*index, range)) {
            Location outside = *base;
            outside.type = *elementType;
            outside.whole = false;
            outside.outOfRange = true;
            return outside;
        }
        return narrowed(*base, positionOf(*index, range) * static_cast<std::int64_t>(stride),
                        std::nullopt, *elementType);
    }

    const std::optional<Value> index = translateExpression(select.selector());
    if (!index.has_value()) return std::nullopt;

    // Straight into a whole object, an index past the dimension is past the object, which a
    // read and a store already handle; inside a window it would land in a neighbour.
    Value offset = positionValue(*index, range, stride);
    if (!base->whole) offset = checkedOffset(offset, *index, range);
    Location result = narrowed(*base, std::nullopt, offset, *elementType);

    if (base->whole && base->inObject() && range.left >= range.right) {
        std::tie(result.reachLow, result.reachWidth) =
            reachOfSelect(select.selector(), range.right, stride, elementType->bitWidth(), false,
                          base->type.bitWidth());
    }
    return result;
}

std::optional<ProcessTranslator::Location>
ProcessTranslator::locateRange(const ast::RangeSelectExpression& select) {
    const std::optional<Location> base = locate(select.value());
    if (!base.has_value()) return std::nullopt;

    const std::optional<eir::Type> partType = module->convertType(*select.type);
    if (!partType.has_value()) {
        module->reportUnsupported(select.sourceRange.start(), "this part select type");
        failed = true;
        return std::nullopt;
    }

    const ast::Type& baseType = *select.value().type;
    const slang::ConstantRange range = baseType.getFixedRange();
    std::uint32_t stride = elementBitsOf(baseType);
    if (baseType.isUnpackedArray()) {
        const std::optional<eir::Type> element =
            module->convertType(*baseType.getArrayElementType());
        if (!element.has_value()) {
            module->reportUnsupported(select.sourceRange.start(), "this element type");
            failed = true;
            return std::nullopt;
        }
        stride = element->bitWidth();
    }
    const auto count = static_cast<std::int64_t>(partType->bitWidth() / std::max(stride, 1U));
    const bool descending = range.left >= range.right;

    // The window's low bit is the position of whichever selected index sits lowest, which
    // depends on which way the dimension runs.
    const auto fromConstant = [&](std::int64_t lowIndex,
                                  std::int64_t highIndex) -> std::optional<Location> {
        const bool lowInside = inRange(lowIndex, range);
        const bool highInside = inRange(highIndex, range);
        if (!lowInside && !highInside) {
            Location outside = *base;
            outside.type = *partType;
            outside.whole = false;
            outside.outOfRange = true;
            return outside;
        }
        const std::int64_t bottom = descending ? lowIndex : highIndex;
        const std::int64_t position = positionOf(bottom, range) * static_cast<std::int64_t>(stride);
        if ((!lowInside || !highInside) && (!base->whole || position < 0)) {
            module->reportUnsupported(select.sourceRange.start(),
                                      "a part select that runs past its declared range here");
            failed = true;
            return std::nullopt;
        }
        return narrowed(*base, position, std::nullopt, *partType);
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
        return fromConstant(std::min(*left, *right), std::max(*left, *right));
    }

    case ast::RangeSelectionKind::IndexedUp:
    case ast::RangeSelectionKind::IndexedDown: {
        const bool up = select.getSelectionKind() == ast::RangeSelectionKind::IndexedUp;
        if (const std::optional<std::int64_t> start = constantValueOf(select.left())) {
            const std::int64_t other = up ? *start + count - 1 : *start - count + 1;
            return fromConstant(std::min(*start, other), std::max(*start, other));
        }

        const std::optional<Value> start = translateExpression(select.left());
        if (!start.has_value()) return std::nullopt;

        // `[b +: n]` holds b to b+n-1 and `[b -: n]` holds b-n+1 to b; the one of those
        // sitting lowest in storage gives the offset.
        Value bottom = widenForOffsetArithmetic(*start);
        if (up != descending) {
            bottom = offsetArithmetic(up ? eir::Opcode::Add : eir::Opcode::Subtract, bottom,
                                      offsetConstant(count - 1));
        }
        Value offset = positionValue(bottom, range, stride);
        if (!base->whole) offset = checkedOffset(offset, bottom, range);
        Location result = narrowed(*base, std::nullopt, offset, *partType);

        if (base->whole && base->inObject() && descending) {
            std::tie(result.reachLow, result.reachWidth) =
                reachOfSelect(select.left(), range.right, stride, partType->bitWidth(), !up,
                              base->type.bitWidth());
        }
        return result;
    }
    }

    module->reportUnsupported(select.sourceRange.start(), "this part select form");
    failed = true;
    return std::nullopt;
}

std::optional<ProcessTranslator::Location>
ProcessTranslator::locateMember(const ast::MemberAccessExpression& access) {
    if (access.member.kind != slang::ast::SymbolKind::Field) {
        module->reportUnsupported(access.sourceRange.start(), "this kind of member access");
        failed = true;
        return std::nullopt;
    }
    const auto& field = access.member.as<ast::FieldSymbol>();

    const std::optional<eir::Type> fieldType = module->convertType(*access.type);
    if (!fieldType.has_value()) {
        module->reportUnsupported(access.sourceRange.start(), "the type of this member");
        failed = true;
        return std::nullopt;
    }

    const std::optional<Location> base = locate(access.value());
    if (!base.has_value()) return std::nullopt;

    // A packed struct's fields carry their offsets. An unpacked one is laid out as a packed one
    // would be, first field highest, so a field sits above every field declared after it.
    auto offset = static_cast<std::int64_t>(field.bitOffset);
    const ast::Type& structType = access.value().type->getCanonicalType();
    if (structType.isUnpackedStruct()) {
        offset = 0;
        for (const ast::FieldSymbol* other : structType.as<ast::UnpackedStructType>().fields) {
            if (other->fieldIndex <= field.fieldIndex) continue;
            const std::optional<eir::Type> otherType = module->convertType(other->getType());
            if (!otherType.has_value()) {
                module->reportUnsupported(access.sourceRange.start(), "the type of this member");
                failed = true;
                return std::nullopt;
            }
            offset += otherType->bitWidth();
        }
    }
    return narrowed(*base, offset, std::nullopt, *fieldType);
}

std::optional<ProcessTranslator::Value> ProcessTranslator::readLocation(const Location& location) {
    const eir::Type type = location.type;

    if (location.outOfRange) {
        // An out-of-range read is the type's default: X when four-state, zero when two-state.
        const std::uint32_t constant = addConstant(eir::BitVector::filled(
            type.bitWidth(), type.isFourState() ? eir::Logic::Unknown : eir::Logic::Zero));
        const std::uint32_t result = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                    eir::Operand::constant(constant)));
        return Value{.reg = result, .type = type};
    }

    if (location.whole) {
        if (!location.inObject())
            return location.rootIsLocal ? copyOf(location.root) : location.root;
        noteRead(location.objectIndex);
        const std::uint32_t result = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::ReadObject, eir::Operand::reg(result),
                                    eir::Operand::object(location.objectIndex)));
        return Value{.reg = result, .type = type};
    }

    const Value low = lowOf(location);
    const std::uint32_t result = newRegister(type);
    if (location.inObject()) {
        noteRead(location.objectIndex);
        emit(eir::Instruction::make(eir::Opcode::ReadObjectPart, eir::Operand::reg(result),
                                    eir::Operand::object(location.objectIndex),
                                    eir::Operand::reg(low.reg)));
    } else {
        emit(eir::Instruction::make(eir::Opcode::PartSelect, eir::Operand::reg(result),
                                    eir::Operand::reg(location.root.reg),
                                    eir::Operand::reg(low.reg)));
    }
    return Value{.reg = result, .type = type};
}

bool ProcessTranslator::writeLocation(const ast::Expression& target, const Location& location,
                                      const Value& value, bool nonBlocking) {
    if (!location.inObject() && location.rootIsLocal) {
        // A window of a local is spliced into its register, which is all a local is.
        if (location.outOfRange) return true;
        const Value stored = coerceState(coerce(value, location.type), location.type);
        if (location.whole) {
            emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(location.root.reg),
                                        eir::Operand::reg(stored.reg)));
            return true;
        }
        const Value low = lowOf(location);
        emit(eir::Instruction::make(eir::Opcode::SplicePart, eir::Operand::reg(location.root.reg),
                                    eir::Operand::reg(stored.reg), eir::Operand::reg(low.reg)));
        return true;
    }

    if (!location.inObject()) {
        module->reportUnsupported(target.sourceRange.start(),
                                  "writing through anything but a plain signal reference");
        failed = true;
        return false;
    }

    // The language discards a write outside its dimension, so it drives nothing.
    if (location.outOfRange) return true;

    const Value stored = coerceState(coerce(value, location.type), location.type);

    if (location.whole) {
        noteWholeWrite(location.objectIndex);
        emit(eir::Instruction::make(
            nonBlocking ? eir::Opcode::StoreNonBlocking : eir::Opcode::Store, {},
            eir::Operand::object(location.objectIndex), eir::Operand::reg(stored.reg)));
        return true;
    }

    // The bits written, for the driver this process is (section 3.7): exactly the window when
    // it is constant, and everything it could reach when it moves.
    const bool written =
        location.constantLow.has_value()
            ? noteWrite(location.objectIndex, *location.constantLow, location.type.bitWidth())
            : noteWrite(location.objectIndex, location.reachLow, location.reachWidth);
    if (!written) return true;

    const Value low = lowOf(location);
    emit(eir::Instruction::make(nonBlocking ? eir::Opcode::StorePartNonBlocking
                                            : eir::Opcode::StorePart,
                                {}, eir::Operand::object(location.objectIndex),
                                eir::Operand::reg(stored.reg), eir::Operand::reg(low.reg)));
    return true;
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

        if (name == "$isunknown" || name == "$countones" || name == "$onehot"
            || name == "$onehot0") {
            return translateBitCount(name, call, expression);
        }

        module->reportUnsupported(expression.sourceRange.start(),
                                  std::string("the system function ") + std::string(name));
        failed = true;
        return std::nullopt;
    }

    return inlineCall(call, expression);
}

const ProcessTranslator::Value* ProcessTranslator::localFor(const ast::ValueSymbol& symbol) const {
    const auto found = locals.find(&symbol);
    return found != locals.end() ? &found->second : nullptr;
}

ProcessTranslator::Value ProcessTranslator::copyOf(const Value& value) {
    const std::uint32_t result = newRegister(value.type);
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(result),
                                eir::Operand::reg(value.reg)));
    return Value{.reg = result, .type = value.type};
}

ProcessTranslator::Value ProcessTranslator::defaultValue(eir::Type type) {
    const std::uint32_t constant = addConstant(eir::BitVector::filled(
        type.bitWidth(), type.isFourState() ? eir::Logic::Unknown : eir::Logic::Zero));
    const std::uint32_t result = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(result),
                                eir::Operand::constant(constant)));
    return Value{.reg = result, .type = type};
}

void ProcessTranslator::bindLocals(const ast::Scope& scope) {
    for (const ast::Symbol& member : scope.members()) {
        if (member.kind == slang::ast::SymbolKind::StatementBlock) {
            bindLocals(member.as<ast::StatementBlockSymbol>());
            continue;
        }
        if (member.kind != slang::ast::SymbolKind::Variable
            && member.kind != slang::ast::SymbolKind::FormalArgument)
            continue;

        const auto& variable = member.as<ast::VariableSymbol>();
        const std::optional<eir::Type> type = module->convertType(variable.getType());
        if (!type.has_value()) {
            module->reportUnsupported(variable.location,
                                      std::format("the type of '{}'", variable.name));
            failed = true;
            return;
        }
        // Every call starts its locals afresh, at their type's default; a declaration with an
        // initialiser sets it where the declaration is (see translateStatement).
        locals.insert_or_assign(&variable, defaultValue(*type));
    }
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::inlineCall(const ast::CallExpression& call, const ast::Expression& expression) {
    const ast::SubroutineSymbol& subroutine = *std::get<0>(call.subroutine);

    if (std::ranges::any_of(inlineFrames, [&subroutine](const InlineFrame& frame) {
            return frame.subroutine == &subroutine;
        })) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  std::format("a recursive call of '{}'", subroutine.name));
        failed = true;
        return std::nullopt;
    }

    // The arguments are evaluated where the call is, before anything of the callee is bound,
    // so that an argument naming a variable the callee also declares means the caller's.
    const auto formals = subroutine.getArguments();
    const auto actuals = call.arguments();
    if (formals.size() != actuals.size()) {
        module->reportUnsupported(expression.sourceRange.start(), "this call's arguments");
        failed = true;
        return std::nullopt;
    }

    // An output's actual is the left side of an assignment slang builds for it.
    const auto targetOf = [](const ast::Expression* actual) -> const ast::Expression* {
        if (actual->kind == ast::ExpressionKind::Assignment)
            return &actual->as<ast::AssignmentExpression>().left();
        return actual;
    };

    std::vector<std::optional<Value>> inputs(formals.size());
    for (std::size_t index = 0; index < formals.size(); ++index) {
        const ast::ArgumentDirection direction = formals[index]->direction;
        if (direction == ast::ArgumentDirection::Ref) {
            module->reportUnsupported(actuals[index]->sourceRange.start(), "a `ref` argument");
            failed = true;
            return std::nullopt;
        }
        if (direction == ast::ArgumentDirection::Out) continue;

        const ast::Expression* source =
            direction == ast::ArgumentDirection::InOut ? targetOf(actuals[index]) : actuals[index];
        inputs[index] = translateExpression(*source);
        if (!inputs[index].has_value()) return std::nullopt;
    }

    // Bind the callee's variables, keeping whatever they shadow for after the call.
    const std::map<const ast::ValueSymbol*, Value> outer = locals;
    bindLocals(subroutine);
    if (failed) return std::nullopt;

    for (std::size_t index = 0; index < formals.size(); ++index) {
        const std::optional<Value>& input = inputs[index];
        if (!input.has_value()) continue;
        const Value& bound = locals.at(formals[index]);
        const Value converted = coerceState(coerce(*input, bound.type), bound.type);
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(bound.reg),
                                    eir::Operand::reg(converted.reg)));
    }

    const std::uint32_t exitBlock = newBlock("return");
    inlineFrames.push_back(InlineFrame{.subroutine = &subroutine,
                                       .exitBlock = exitBlock,
                                       .returnVariable = subroutine.returnValVar});
    const bool translated = translateStatement(subroutine.getBody());
    inlineFrames.pop_back();
    if (!translated) return std::nullopt;
    if (!currentBlockTerminated())
        emit(eir::Instruction::make(eir::Opcode::Jump, {}, eir::Operand::block(exitBlock)));
    selectBlock(exitBlock);

    // What the call returns and what it writes back are read before the callee's bindings
    // are dropped; the outputs are assigned with the caller's bindings back in place, since
    // their targets are the caller's.
    std::optional<Value> result;
    if (subroutine.returnValVar != nullptr) {
        if (const Value* returned = localFor(*subroutine.returnValVar)) result = copyOf(*returned);
    }
    std::vector<std::pair<std::size_t, Value>> outputs;
    for (std::size_t index = 0; index < formals.size(); ++index) {
        const ast::ArgumentDirection direction = formals[index]->direction;
        if (direction == ast::ArgumentDirection::Out || direction == ast::ArgumentDirection::InOut)
            outputs.emplace_back(index, copyOf(locals.at(formals[index])));
    }
    locals = outer;

    for (const auto& [index, value] : outputs) {
        if (!assignTo(*targetOf(actuals[index]), value, false)) return std::nullopt;
    }

    if (result.has_value()) return result;
    // A task or a void function produces nothing; a placeholder keeps the callers that expect
    // a value, an expression statement among them, from having to tell two absences apart.
    return defaultValue(eir::Type::logic());
}

std::optional<ProcessTranslator::Value>
ProcessTranslator::translateBitCount(std::string_view name, const ast::CallExpression& call,
                                     const ast::Expression& expression) {
    if (call.arguments().size() != 1) {
        module->reportUnsupported(expression.sourceRange.start(),
                                  std::string(name) + " with other than one argument");
        failed = true;
        return std::nullopt;
    }
    const std::optional<Value> operand = translateExpression(*call.arguments()[0]);
    if (!operand.has_value()) return std::nullopt;

    const std::optional<eir::Type> declared = module->convertType(*expression.type);
    const eir::Type resultType = declared.value_or(eir::Type::bit());

    const auto loadConstant = [&](std::uint64_t value, eir::Type type) {
        const std::uint32_t index = addConstant(eir::BitVector::fromUInt64(value, type.bitWidth()));
        const std::uint32_t reg = newRegister(type);
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(reg),
                                    eir::Operand::constant(index)));
        return Value{.reg = reg, .type = type};
    };

    if (name == "$isunknown") {
        // A reduction XOR is X exactly when some bit is X or Z, and `===` asks which.
        const std::uint32_t reduced = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::ReduceXor, eir::Operand::reg(reduced),
                                    eir::Operand::reg(operand->reg)));
        const eir::BitVector unknown(1);
        const std::uint32_t index = addConstant(unknown);
        const std::uint32_t unknownReg = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(unknownReg),
                                    eir::Operand::constant(index)));
        const std::uint32_t result = newRegister(eir::Type::logic());
        emit(eir::Instruction::make(eir::Opcode::CaseEqual, eir::Operand::reg(result),
                                    eir::Operand::reg(reduced), eir::Operand::reg(unknownReg)));
        return coerce(Value{.reg = result, .type = eir::Type::logic()}, resultType);
    }

    const eir::Type countType = eir::Type::intType();
    const std::uint32_t count = newRegister(countType);
    emit(eir::Instruction::make(eir::Opcode::CountOnes, eir::Operand::reg(count),
                                eir::Operand::reg(operand->reg)));
    const Value counted{.reg = count, .type = countType};

    if (name == "$countones") return coerce(counted, resultType);

    // `$onehot` is exactly one bit set and `$onehot0` at most one; the count is two-state, so
    // the comparison never yields X.
    const Value one = loadConstant(1, countType);
    const Value matched =
        compare(name == "$onehot" ? eir::Opcode::Equal : eir::Opcode::LessEqual, counted, one);
    return coerce(matched, resultType);
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
        if (const Value* local = localFor(target.as<ast::ValueExpressionBase>().symbol)) {
            const Value stored = coerceState(coerce(value, local->type), local->type);
            emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(local->reg),
                                        eir::Operand::reg(stored.reg)));
            return true;
        }

        const std::optional<std::uint32_t> objectIndex =
            module->objectFor(target.as<ast::ValueExpressionBase>().symbol);
        if (!objectIndex.has_value()) {
            failed = true;
            return false;
        }

        const eir::Type objectType = module->definition().objects[*objectIndex].type;
        const Value widened = coerce(value, objectType);
        const Value stored = coerceState(widened, objectType);

        noteWholeWrite(*objectIndex);
        emit(eir::Instruction::make(
            nonBlocking ? eir::Opcode::StoreNonBlocking : eir::Opcode::Store, {},
            eir::Operand::object(*objectIndex), eir::Operand::reg(stored.reg)));
        return true;
    }

    case ast::ExpressionKind::ElementSelect:
    case ast::ExpressionKind::RangeSelect:
    case ast::ExpressionKind::MemberAccess: {
        const std::optional<Location> location = locate(target);
        if (!location.has_value()) return false;
        return writeLocation(target, *location, value, nonBlocking);
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

namespace {

/// The string literal `expression` is, under any conversions slang wrapped it in, or null.
///
/// A literal passed where a `string` is expected arrives converted to one, and a system task
/// that reads its format or file name from a literal has to see through that.
const ast::StringLiteral* stringLiteralOf(const ast::Expression& expression) {
    const ast::Expression* current = &expression;
    while (current->kind == ast::ExpressionKind::Conversion)
        current = &current->as<ast::ConversionExpression>().operand();
    return current->kind == ast::ExpressionKind::StringLiteral ? &current->as<ast::StringLiteral>()
                                                               : nullptr;
}

} // namespace

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
        // `$stop` would return control to an interactive prompt, and there is none. Treating
        // it as `$finish` ends the run instead of ignoring it, which is the safer of the two
        // wrong answers.
        kind = eir::SysTaskKind::Finish;
        ignoreArguments = true;
    } else if (name == "$fatal") {
        kind = eir::SysTaskKind::Fatal;
        // `$fatal`'s first argument is the finish code, not part of the message.
        skipFirstArgument =
            !call.arguments().empty() && stringLiteralOf(*call.arguments()[0]) == nullptr;
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
            if (const ast::StringLiteral* literal = stringLiteralOf(*argument);
                first && literal != nullptr) {
                descriptor.formatIndex = addString(std::string(literal->getValue()));
                first = false;
                continue;
            }
            first = false;

            const std::optional<Value> value = translateExpression(*argument);
            if (!value.has_value()) return false;

            descriptor.arguments.push_back(
                eir::SysTaskArgument{.value = eir::Operand::reg(value->reg), .type = value->type});
        }
    } else if (!arguments.empty() && stringLiteralOf(*arguments[0]) != nullptr) {
        descriptor.formatIndex = addString(std::string(stringLiteralOf(*arguments[0])->getValue()));
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
