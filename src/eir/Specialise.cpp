// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Specialise.h"

#include "eir/Process.h"

#include <algorithm>
#include <utility>

namespace enzan::eir {

namespace {

SpecialiseFailure evaluationFailure(const ParameterEvaluator& evaluator, SourceLocation location,
                                    std::optional<ParameterError> own = std::nullopt) {
    return SpecialiseFailure{.kind = SpecialiseFailure::Kind::Evaluation,
                             .location = location,
                             .evaluation = own.value_or(evaluator.error())};
}

void resize(Type& type, std::uint32_t width) {
    if (type.isBits()) type = type.withWidth(width);
}

} // namespace

std::variant<std::vector<std::optional<ParameterValue>>, SpecialiseFailure>
formalValues(const ModuleDefinition& unit,
             const std::map<std::string, ParameterValue, std::less<>>& actuals,
             const InterfaceParameters& interfaces) {
    for (const auto& [name, value] : actuals) {
        const bool known = std::ranges::any_of(unit.formals, [&](const ParameterFormal& formal) {
            return formal.kind == FormalKind::Parameter && formal.name == name;
        });
        if (!known) {
            return SpecialiseFailure{.kind = SpecialiseFailure::Kind::UnknownActual,
                                     .formal = name,
                                     .location = unit.location};
        }
    }

    std::vector<std::optional<ParameterValue>> values(unit.formals.size());
    ParameterEvaluator evaluator(unit.expressions, values);

    for (std::size_t index = 0; index < unit.formals.size(); ++index) {
        const ParameterFormal& formal = unit.formals[index];

        std::optional<ParameterValue> given;
        if (formal.kind == FormalKind::InterfaceParameter) {
            if (interfaces) given = interfaces(formal.port, formal.name);
            if (!given.has_value()) {
                return SpecialiseFailure{.kind = SpecialiseFailure::Kind::NoValue,
                                         .formal = formal.port + "." + formal.name,
                                         .location = formal.location};
            }
        } else if (const auto actual = actuals.find(formal.name);
                   formal.kind == FormalKind::Parameter && actual != actuals.end()) {
            given = actual->second;
        } else if (formal.value != NoExpression) {
            const ParameterValue* value = evaluator.value(formal.value);
            if (value == nullptr) return evaluationFailure(evaluator, formal.location);
            given = *value;
        } else {
            return SpecialiseFailure{.kind = SpecialiseFailure::Kind::NoValue,
                                     .formal = formal.name,
                                     .location = formal.location};
        }

        std::uint32_t width = formal.width;
        if (formal.widthExpression != NoExpression) {
            const std::optional<std::uint32_t> evaluated =
                evaluator.integer(formal.widthExpression);
            if (!evaluated.has_value()) return evaluationFailure(evaluator, formal.location);
            if (*evaluated == 0)
                return evaluationFailure(evaluator, formal.location, ParameterError::ZeroWidth);
            width = *evaluated;
        }

        if (formal.isTyped) {
            given = ParameterValue{.bits = given->bits.resized(width, given->isSigned),
                                   .isSigned = formal.isSigned};
        } else if (given->bits.width() != width || given->isSigned != formal.isSigned) {
            return SpecialiseFailure{.kind = SpecialiseFailure::Kind::UntypedMismatch,
                                     .formal = formal.name,
                                     .location = formal.location,
                                     .givenWidth = given->bits.width(),
                                     .givenSigned = given->isSigned,
                                     .expectedWidth = width,
                                     .expectedSigned = formal.isSigned};
        }
        values[index] = std::move(given);
    }
    return values;
}

std::variant<ModuleDefinition, SpecialiseFailure>
specialiseAt(const ModuleDefinition& unit, std::span<const std::optional<ParameterValue>> values) {
    ParameterEvaluator evaluator(unit.expressions, values);
    std::optional<ParameterError> failure;

    const auto integer = [&](std::uint32_t expression, std::uint32_t& into) {
        if (expression == NoExpression) return true;
        const std::optional<std::uint32_t> value = evaluator.integer(expression);
        if (!value.has_value()) return false;
        into = *value;
        return true;
    };
    const auto width = [&](std::uint32_t expression, std::uint32_t& into) {
        if (expression == NoExpression) return true;
        if (!integer(expression, into)) return false;
        if (into > 0) return true;
        failure = ParameterError::ZeroWidth;
        return false;
    };
    const auto bound = [&](std::uint32_t expression, std::int32_t& into) {
        if (expression == NoExpression) return true;
        const ParameterValue* value = evaluator.value(expression);
        if (value == nullptr) return false;
        if (value->bits.ref().hasUnknown()) {
            failure = ParameterError::NotAnInteger;
            return false;
        }
        into = static_cast<std::int32_t>(
            value->isSigned ? value->bits.ref().toInt64()
                            : static_cast<std::int64_t>(value->bits.ref().toUInt64()));
        return true;
    };
    // A type whose width is an expression's value takes it, and is concrete from then on.
    const auto concrete = [&](Type& type) {
        std::uint32_t typeWidth = type.bitWidth();
        if (!width(type.widthExpression(), typeWidth)) return false;
        resize(type, typeWidth);
        return true;
    };
    const auto bits = [&](std::uint32_t expression, BitVector& into) {
        if (expression == NoExpression) return true;
        const ParameterValue* value = evaluator.value(expression);
        if (value == nullptr) return false;
        into = value->bits;
        return true;
    };

    ModuleDefinition result = unit;
    bool evaluated = true;
    for (Object& object : result.objects) {
        evaluated = evaluated && concrete(object.type);
        evaluated = evaluated && width(object.elementWidthExpression, object.elementWidth);
        for (UnpackedDimension& dimension : object.unpackedDimensions) {
            evaluated = evaluated && bound(dimension.leftExpression, dimension.left)
                        && bound(dimension.rightExpression, dimension.right);
            dimension.leftExpression = dimension.rightExpression = NoExpression;
        }
        if (object.initialValueExpression != NoExpression) {
            BitVector value(1);
            evaluated = evaluated && bits(object.initialValueExpression, value);
            object.initialValue = std::move(value);
        }
        object.elementWidthExpression = object.initialValueExpression = NoExpression;
    }

    for (Process& process : result.processes) {
        for (Type& type : process.registerTypes) evaluated = evaluated && concrete(type);
        for (std::size_t constant = 0; constant < process.constantExpressions.size(); ++constant)
            evaluated = evaluated
                        && bits(process.constantExpressions[constant], process.constants[constant]);
        for (SysTaskCall& call : process.sysCalls)
            for (SysTaskArgument& argument : call.arguments)
                evaluated = evaluated && concrete(argument.type);

        const auto entry = [&](SensitivityEntry& sensitivity) {
            const bool ok = integer(sensitivity.lowBitExpression, sensitivity.lowBit)
                            && width(sensitivity.widthExpression, sensitivity.width);
            sensitivity.lowBitExpression = sensitivity.widthExpression = NoExpression;
            return ok;
        };
        for (SensitivityEntry& sensitivity : process.sensitivity)
            evaluated = evaluated && entry(sensitivity);
        for (std::vector<SensitivityEntry>& list : process.eventLists)
            for (SensitivityEntry& sensitivity : list) evaluated = evaluated && entry(sensitivity);
        for (WriteExtent& extent : process.writeSet) {
            evaluated = evaluated && integer(extent.lowBitExpression, extent.lowBit)
                        && width(extent.widthExpression, extent.width);
            extent.lowBitExpression = extent.widthExpression = NoExpression;
        }
        normaliseWriteSet(process.writeSet);

        process.constantExpressions.clear();
    }

    // Each instance's actuals become constants of the result's own table, the only expressions
    // a concrete unit holds.
    ParameterExpressions constants;
    for (Instance& instance : result.instances) {
        for (ParameterActual& actual : instance.parameterActuals) {
            const ParameterValue* value = evaluator.value(actual.value);
            if (value == nullptr) {
                evaluated = false;
                continue;
            }
            actual.value = constants.constant(value->bits, value->isSigned);
        }
    }
    if (!evaluated) return evaluationFailure(evaluator, unit.location, failure);

    // What a reader is shown for a parameter is what it resolved to here. One the unit records
    // that is no formal — a localparam inside a generate block — keeps the representative's
    // value, which is what makes a unit that has one compare unequal to its own specialisation.
    for (Parameter& parameter : result.parameters) {
        const auto formal = std::ranges::find(unit.formals, parameter.name, &ParameterFormal::name);
        if (formal == unit.formals.end() || formal->kind == FormalKind::InterfaceParameter)
            continue;
        const auto index = static_cast<std::size_t>(formal - unit.formals.begin());
        if (index < values.size() && values[index].has_value())
            parameter.value = values[index]->bits;
    }

    result.formals.clear();
    result.expressions = std::move(constants);
    return result;
}

std::map<std::string, ParameterValue, std::less<>> actualValues(const ModuleDefinition& unit,
                                                                const Instance& instance) {
    std::map<std::string, ParameterValue, std::less<>> result;
    ParameterEvaluator evaluator(unit.expressions, {});
    for (const ParameterActual& actual : instance.parameterActuals) {
        if (const ParameterValue* value = evaluator.value(actual.value))
            result.insert_or_assign(actual.formal, *value);
    }
    return result;
}

} // namespace enzan::eir
