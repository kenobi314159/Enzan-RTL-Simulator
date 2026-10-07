// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Specialiser.h"

#include "common/Diagnostics.h"
#include "eir/ParameterExpression.h"
#include "eir/Verifier.h"
#include "elab/JoinNames.h"

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>
#include <string_view>
#include <utility>

namespace enzan::elab {

namespace {

using ValueMap = std::map<std::string, eir::ParameterValue, std::less<>>;
using InterfaceMap = std::map<std::string, Specialiser::Interface, std::less<>>;

std::string_view describe(eir::ParameterError error) {
    switch (error) {
    case eir::ParameterError::NotAnInteger:
        return "a width, a count or a bit position came out unknown or negative";
    case eir::ParameterError::TooWide: return "a width came out wider than any vector can be";
    case eir::ParameterError::ZeroWidth: return "a width came out zero";
    case eir::ParameterError::EmptyReplication:
        return "a replication of zero copies stands outside a concatenation";
    case eir::ParameterError::SelectOutOfRange: return "a part select reaches outside its value";
    case eir::ParameterError::MissingFormal:
        return "an expression uses a parameter that has no value yet";
    }
    return "an expression has no value";
}

/// How a type is described to a reader comparing two of them.
std::string describeType(std::uint32_t width, bool isSigned) {
    return std::format("{}-bit {} value", width, isSigned ? "signed" : "unsigned");
}

/// The name an instance uses for the unit `name` of `library`: qualified by its library when
/// it has one, so that two libraries' specialisations of one name stay apart (section 8.2).
std::string referenceTo(const std::string& library, const std::string& name) {
    return library.empty() ? name : std::format("{}.{}", library, name);
}

} // namespace

Specialiser::Specialiser(const eir::Design& analysedDesign, DiagnosticEngine& engine) noexcept
    : analysed(analysedDesign), diagnostics(engine) {}

std::optional<Specialiser::Result> Specialiser::specialise(std::uint32_t top) {
    units = std::make_unique<eir::Design>();
    const eir::ModuleDefinition& topDefinition = analysed.module(top);
    const std::string& topName =
        topDefinition.sourceName.empty() ? topDefinition.name : topDefinition.sourceName;
    const std::optional<std::string> name = unitFor(top, "/" + topName, {}, {});
    if (!name.has_value()) return std::nullopt;

    // A package's variables are instantiated at the root when something refers to one, rather
    // than by an instance, so no instance reaches them; they are never generic.
    for (const eir::ModuleDefinition& definition : analysed.modules())
        if (definition.isPackage
            && units->findModule(referenceTo(definition.library, definition.name)) == nullptr)
            units->addModule(definition);

    const std::optional<std::uint32_t> index = units->findModuleIndex(*name);
    if (!index.has_value()) return std::nullopt;
    return Result{.units = std::move(units), .top = *index};
}

std::optional<std::string> Specialiser::unitFor(std::uint32_t index,
                                                const std::string& instancePath,
                                                const ValueMap& actuals,
                                                const InterfaceMap& interfaces) {
    const eir::ModuleDefinition& definition = analysed.module(index);
    const std::string& sourceName =
        definition.sourceName.empty() ? definition.name : definition.sourceName;

    // The formals' values, in the order they are declared, each from what came before it.
    std::vector<std::optional<eir::ParameterValue>> formals(definition.formals.size());
    eir::ParameterEvaluator evaluator(definition.expressions, formals);
    // Why evaluation failed when the evaluator itself did not: a width that is a fine integer
    // and zero.
    std::optional<eir::ParameterError> failure;
    const auto evaluationFailed = [&]() {
        diagnostics.report(DiagCode::ParameterEvaluationFailed, definition.location, sourceName,
                           instancePath, describe(failure.value_or(evaluator.error())));
        return std::nullopt;
    };

    for (const auto& [name, value] : actuals) {
        const bool known = std::ranges::any_of(definition.formals, [&](const auto& formal) {
            return formal.kind == eir::FormalKind::Parameter && formal.name == name;
        });
        if (!known) {
            diagnostics.report(DiagCode::UnknownParameterActual, definition.location, instancePath,
                               name, sourceName);
            return std::nullopt;
        }
    }

    std::vector<eir::NamedValue> nameValues;
    for (std::size_t formalIndex = 0; formalIndex < definition.formals.size(); ++formalIndex) {
        const eir::ParameterFormal& formal = definition.formals[formalIndex];

        std::optional<eir::ParameterValue> given;
        if (formal.kind == eir::FormalKind::InterfaceParameter) {
            const auto interface = interfaces.find(formal.port);
            if (interface != interfaces.end()) {
                const auto found = interface->second.parameters.find(formal.name);
                if (found != interface->second.parameters.end()) given = found->second;
            }
            if (!given.has_value()) {
                diagnostics.report(DiagCode::ParameterHasNoValue, formal.location,
                                   std::format("{}.{}", formal.port, formal.name), sourceName,
                                   instancePath);
                return std::nullopt;
            }
        } else if (const auto actual = actuals.find(formal.name);
                   formal.kind == eir::FormalKind::Parameter && actual != actuals.end()) {
            given = actual->second;
        } else if (formal.value != eir::NoExpression) {
            const eir::ParameterValue* value = evaluator.value(formal.value);
            if (value == nullptr) return evaluationFailed();
            given = *value;
        } else {
            diagnostics.report(DiagCode::ParameterHasNoValue, formal.location, formal.name,
                               sourceName, instancePath);
            return std::nullopt;
        }

        std::uint32_t width = formal.width;
        if (formal.widthExpression != eir::NoExpression) {
            const std::optional<std::uint32_t> evaluated =
                evaluator.integer(formal.widthExpression);
            if (evaluated == 0U) failure = eir::ParameterError::ZeroWidth;
            if (!evaluated.has_value() || *evaluated == 0) return evaluationFailed();
            width = *evaluated;
        }

        // A typed formal converts what it is given, as an assignment would. An untyped one is
        // its value's type, and every width the unit was translated with assumed the
        // representative's; another type is a different unit, which only slang can produce.
        if (formal.isTyped) {
            given = eir::ParameterValue{.bits = given->bits.resized(width, given->isSigned),
                                        .isSigned = formal.isSigned};
        } else if (given->bits.width() != width || given->isSigned != formal.isSigned) {
            diagnostics.report(DiagCode::UntypedParameterType, formal.location, formal.name,
                               sourceName, instancePath,
                               describeType(given->bits.width(), given->isSigned),
                               describeType(width, formal.isSigned), sourceName);
            return std::nullopt;
        }

        formals[formalIndex] = std::move(given);
        if (formal.kind == eir::FormalKind::Parameter) {
            nameValues.push_back(eir::NamedValue{
                .name = formal.name, .value = eir::renderParameterValue(*formals[formalIndex])});
        }
    }

    // What an interface port reaches is part of the name wherever it can change the unit: a
    // generic unit reads its parameters, and one that passes the port down to a generic child
    // makes that child's specialisation, and so its own instance list, depend on it.
    const bool passesInterfaces = std::ranges::any_of(definition.instances, [&](const auto& child) {
        const eir::ModuleDefinition* target = analysed.findModule(child.definitionName);
        return target != nullptr && target->isGeneric()
               && std::ranges::any_of(child.interfaceBindings, [](const auto& binding) {
                      return binding.target.starts_with('@');
                  });
    });
    if (definition.isGeneric() || passesInterfaces) {
        for (const auto& [port, interface] : interfaces) {
            nameValues.push_back(eir::NamedValue{
                .name = port,
                .value = interface.elements == 0
                             ? interface.unit
                             : std::format("{}[{}]", interface.unit, interface.elements)});
        }
    }

    const std::string name = definition.isGeneric() || passesInterfaces
                                 ? eir::specialisedName(sourceName, nameValues)
                                 : definition.name;

    const std::string reference = referenceTo(definition.library, name);
    if (units->findModule(reference) != nullptr) return reference;
    if (std::ranges::find(inProgress, name) != inProgress.end()) {
        diagnostics.report(DiagCode::RecursiveInstantiation, definition.location, sourceName,
                           instancePath);
        return std::nullopt;
    }
    inProgress.push_back(name);

    eir::ModuleDefinition unit = definition;
    unit.name = name;

    // Every annotated number becomes its expression's value.
    const auto integer = [&](std::uint32_t expression, std::uint32_t& into) {
        if (expression == eir::NoExpression) return true;
        const std::optional<std::uint32_t> value = evaluator.integer(expression);
        if (!value.has_value()) return false;
        into = *value;
        return true;
    };
    const auto width = [&](std::uint32_t expression, std::uint32_t& into) {
        if (expression == eir::NoExpression) return true;
        if (!integer(expression, into)) return false;
        if (into > 0) return true;
        failure = eir::ParameterError::ZeroWidth;
        return false;
    };
    const auto bound = [&](std::uint32_t expression, std::int32_t& into) {
        if (expression == eir::NoExpression) return true;
        const eir::ParameterValue* value = evaluator.value(expression);
        if (value == nullptr || value->bits.ref().hasUnknown()) return false;
        into = static_cast<std::int32_t>(
            value->isSigned ? value->bits.ref().toInt64()
                            : static_cast<std::int64_t>(value->bits.ref().toUInt64()));
        return true;
    };
    const auto bits = [&](std::uint32_t expression, eir::BitVector& into) {
        if (expression == eir::NoExpression) return true;
        const eir::ParameterValue* value = evaluator.value(expression);
        if (value == nullptr) return false;
        into = value->bits;
        return true;
    };
    const auto resize = [](eir::Type& type, std::uint32_t newWidth) {
        if (type.isBits()) type = eir::Type::bits(newWidth, type.isSigned(), type.isFourState());
    };

    bool evaluated = true;
    for (eir::Object& object : unit.objects) {
        std::uint32_t objectWidth = object.type.bitWidth();
        evaluated = evaluated && width(object.widthExpression, objectWidth);
        resize(object.type, objectWidth);
        evaluated = evaluated && width(object.elementWidthExpression, object.elementWidth);
        for (eir::UnpackedDimension& dimension : object.unpackedDimensions) {
            evaluated = evaluated && bound(dimension.leftExpression, dimension.left)
                        && bound(dimension.rightExpression, dimension.right);
        }
        if (object.initialValueExpression != eir::NoExpression) {
            eir::BitVector value(1);
            evaluated = evaluated && bits(object.initialValueExpression, value);
            object.initialValue = std::move(value);
        }
    }

    for (eir::Process& process : unit.processes) {
        for (std::size_t reg = 0; reg < process.registerWidthExpressions.size(); ++reg) {
            std::uint32_t registerWidth = process.registerTypes[reg].bitWidth();
            evaluated = evaluated && width(process.registerWidthExpressions[reg], registerWidth);
            resize(process.registerTypes[reg], registerWidth);
        }
        for (std::size_t constant = 0; constant < process.constantExpressions.size(); ++constant)
            evaluated = evaluated
                        && bits(process.constantExpressions[constant], process.constants[constant]);

        // A system task's argument is as wide as the register it passes, which may be a
        // parameter's width; its signedness is the argument's own, as the format reads it.
        for (eir::SysTaskCall& call : process.sysCalls) {
            for (eir::SysTaskArgument& argument : call.arguments) {
                if (argument.value.kind() != eir::Operand::Kind::Register
                    || argument.value.index() >= process.registerWidthExpressions.size()
                    || process.registerWidthExpressions[argument.value.index()]
                           == eir::NoExpression)
                    continue;
                resize(argument.type, process.registerTypes[argument.value.index()].bitWidth());
            }
        }

        const auto entry = [&](eir::SensitivityEntry& sensitivity) {
            return integer(sensitivity.lowBitExpression, sensitivity.lowBit)
                   && width(sensitivity.widthExpression, sensitivity.width);
        };
        for (eir::SensitivityEntry& sensitivity : process.sensitivity)
            evaluated = evaluated && entry(sensitivity);
        for (std::vector<eir::SensitivityEntry>& list : process.eventLists)
            for (eir::SensitivityEntry& sensitivity : list)
                evaluated = evaluated && entry(sensitivity);
        for (eir::WriteExtent& extent : process.writeSet) {
            evaluated = evaluated && integer(extent.lowBitExpression, extent.lowBit)
                        && width(extent.widthExpression, extent.width);
        }
        eir::normaliseWriteSet(process.writeSet);

        // The annotations have been applied; what remains is a concrete process.
        process.registerWidthExpressions.clear();
        process.constantExpressions.clear();
        for (eir::SensitivityEntry& sensitivity : process.sensitivity)
            sensitivity.lowBitExpression = sensitivity.widthExpression = eir::NoExpression;
        for (std::vector<eir::SensitivityEntry>& list : process.eventLists)
            for (eir::SensitivityEntry& sensitivity : list)
                sensitivity.lowBitExpression = sensitivity.widthExpression = eir::NoExpression;
        for (eir::WriteExtent& extent : process.writeSet)
            extent.lowBitExpression = extent.widthExpression = eir::NoExpression;
    }
    if (!evaluated) {
        inProgress.pop_back();
        return evaluationFailed();
    }
    for (eir::Object& object : unit.objects) {
        object.widthExpression = object.elementWidthExpression = object.initialValueExpression =
            eir::NoExpression;
        for (eir::UnpackedDimension& dimension : object.unpackedDimensions)
            dimension.leftExpression = dimension.rightExpression = eir::NoExpression;
    }

    // What a reader is shown for this specialisation's parameters.
    if (definition.isGeneric()) {
        unit.parameters.clear();
        for (std::size_t formalIndex = 0; formalIndex < definition.formals.size(); ++formalIndex) {
            const eir::ParameterFormal& formal = definition.formals[formalIndex];
            if (formal.kind == eir::FormalKind::InterfaceParameter) continue;
            unit.parameters.push_back(
                eir::Parameter{.name = formal.name,
                               .value = formals[formalIndex]->bits,
                               .isLocal = formal.kind == eir::FormalKind::LocalParameter,
                               .location = formal.location});
        }
    }
    ValueMap& ownParameters = parametersOf[name];
    for (const eir::Parameter& parameter : unit.parameters) {
        const auto formal =
            std::ranges::find(definition.formals, parameter.name, &eir::ParameterFormal::name);
        ownParameters.insert_or_assign(
            parameter.name,
            eir::ParameterValue{.bits = parameter.value,
                                .isSigned =
                                    formal != definition.formals.end() ? formal->isSigned : true});
    }

    // Each instance names the specialisation it instantiates. Interfaces first where an
    // instance is connected to one, so that its parameters are known when the instance is.
    std::vector<std::optional<std::string>> resolved(unit.instances.size());
    std::vector<bool> resolving(unit.instances.size(), false);
    std::function<bool(std::size_t)> resolve;

    const auto interfaceFor = [&](const std::string& target) -> std::optional<Interface> {
        if (target.starts_with('@')) {
            const std::string_view port = std::string_view(target).substr(
                1,
                target.find('[') == std::string::npos ? std::string::npos : target.find('[') - 1);
            const auto found = interfaces.find(port);
            if (found == interfaces.end()) return std::nullopt;
            Interface interface = found->second;
            if (target.find('[') != std::string::npos) interface.elements = 0;
            return interface;
        }
        std::uint32_t elements = 0;
        std::optional<std::size_t> first;
        for (std::size_t child = 0; child < unit.instances.size(); ++child) {
            const std::string& childName = unit.instances[child].name;
            if (childName == target) {
                first = child;
                break;
            }
            if (childName.starts_with(target + "[")) {
                if (!first.has_value()) first = child;
                ++elements;
            }
        }
        if (!first.has_value() || !resolve(*first)) return std::nullopt;
        return Interface{.unit = *resolved[*first],
                         .parameters = parametersOf[*resolved[*first]],
                         .elements = elements};
    };

    resolve = [&](std::size_t childIndex) -> bool {
        if (resolved[childIndex].has_value()) return true;
        if (resolving[childIndex]) return false;
        resolving[childIndex] = true;

        eir::Instance& child = unit.instances[childIndex];
        const std::string childPath = instancePath + "/" + child.name;

        std::vector<std::string_view> ambiguity;
        const std::optional<std::uint32_t> childIndexInDesign =
            analysed.findModuleIndex(child.definitionName, &ambiguity);
        if (!childIndexInDesign.has_value()) {
            // Left as it is, for elaboration to report as it reports any unresolved instance.
            if (!ambiguity.empty()) {
                diagnostics.report(DiagCode::AmbiguousModule, child.location, child.definitionName,
                                   joinNames(ambiguity), child.definitionName);
                return false;
            }
            resolved[childIndex] = child.definitionName;
            return true;
        }

        ValueMap childActuals;
        for (const eir::ParameterActual& actual : child.parameterActuals) {
            const eir::ParameterValue* value = evaluator.value(actual.value);
            if (value == nullptr) return false;
            childActuals.insert_or_assign(actual.formal, *value);
        }
        InterfaceMap childInterfaces;
        for (const eir::InterfaceBinding& binding : child.interfaceBindings) {
            std::optional<Interface> interface = interfaceFor(binding.target);
            if (interface.has_value())
                childInterfaces.insert_or_assign(binding.port, std::move(*interface));
        }

        const std::optional<std::string> childName =
            unitFor(*childIndexInDesign, childPath, childActuals, childInterfaces);
        if (!childName.has_value()) return false;
        resolved[childIndex] = *childName;
        return true;
    };

    bool instancesResolved = true;
    for (std::size_t child = 0; child < unit.instances.size() && instancesResolved; ++child)
        instancesResolved = resolve(child);
    inProgress.pop_back();
    if (!instancesResolved) return std::nullopt;

    for (std::size_t child = 0; child < unit.instances.size(); ++child) {
        unit.instances[child].definitionName = *resolved[child];
        unit.instances[child].parameterActuals.clear();
    }
    unit.formals.clear();
    unit.expressions = {};

    // A specialisation is checked as a frontend's unit is: an annotation the frontend got wrong
    // shows up here as a unit that does not verify, not as a simulation that is quietly wrong.
    if (definition.isGeneric() && !eir::verify(unit, nullptr, diagnostics)) return std::nullopt;

    units->addModule(std::move(unit));
    return reference;
}

} // namespace enzan::elab
