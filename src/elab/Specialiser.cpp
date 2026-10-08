// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Specialiser.h"

#include "common/Diagnostics.h"
#include "eir/ParameterExpression.h"
#include "eir/Specialise.h"
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
    case eir::ParameterError::CallFailed: return "a constant function could not be run";
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

Specialiser::Specialiser(const eir::Design& analysedDesign, DiagnosticEngine& engine,
                         eir::ConstantFunctions* runner) noexcept
    : analysed(analysedDesign), diagnostics(engine), functions(runner) {}

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

bool Specialiser::adaptPorts(eir::ModuleDefinition& unit, std::size_t instanceIndex) {
    eir::Instance& instance = unit.instances[instanceIndex];
    const eir::ModuleDefinition* child = units->findModule(instance.definitionName);
    if (child == nullptr) return true;

    for (eir::PortConnection& connection : instance.connections) {
        if (!connection.connected() || connection.portIndex >= child->ports.size()) continue;
        const eir::Port& port = child->ports[connection.portIndex];
        const eir::Object& inner = child->objects[port.objectIndex];
        const eir::Object& outer = unit.objects[connection.objectIndex];
        if (!inner.type.isBits() || !outer.type.isBits()
            || inner.type.bitWidth() == outer.type.bitWidth())
            continue;

        // The widths a frontend made equal can differ once a generic unit is specialised, on
        // either side: a connection is then an assignment, as one written out would be (IEEE
        // 1800 section 23.3.3.7), into an object of the port's width that the port collapses
        // onto, the frontend's own shape for such a connection.
        if (port.direction == eir::PortDirection::InOut) {
            diagnostics.report(DiagCode::PortWidthMismatch, port.location, port.name, child->name,
                               inner.type.bitWidth(), outer.type.bitWidth());
            return false;
        }
        const bool input = port.direction == eir::PortDirection::Input;

        eir::Object adapted;
        adapted.name = std::format("{}${}", instance.name, port.name);
        while (unit.findObject(adapted.name).has_value()) adapted.name += '$';
        adapted.type = inner.type;
        adapted.kind = inner.kind;
        adapted.netKind = inner.netKind;
        adapted.location = instance.location;
        const std::uint32_t adaptedIndex = unit.addObject(std::move(adapted));
        const std::uint32_t outerIndex = connection.objectIndex;
        connection.objectIndex = adaptedIndex;

        const std::uint32_t from = input ? outerIndex : adaptedIndex;
        const std::uint32_t to = input ? adaptedIndex : outerIndex;
        const eir::Type fromType = unit.objects[from].type;
        const eir::Type toType = unit.objects[to].type;

        eir::Process process;
        process.kind = eir::ProcessKind::ContinuousAssign;
        process.name = std::format("connection@{}.{}", instance.name, port.name);
        process.location = instance.location;
        process.registerTypes = {fromType, toType};
        process.readSet = {from};
        process.writeSet = {eir::WriteExtent::whole(to, toType.bitWidth())};
        eir::BasicBlock entry;
        entry.label = "entry";
        entry.instructions.push_back(eir::Instruction::make(
            eir::Opcode::ReadObject, eir::Operand::reg(0), eir::Operand::object(from)));
        eir::Instruction convert = eir::Instruction::make(
            toType.bitWidth() < fromType.bitWidth() ? eir::Opcode::Truncate : eir::Opcode::Extend,
            eir::Operand::reg(1), eir::Operand::reg(0));
        convert.isSigned = fromType.isSigned();
        entry.instructions.push_back(convert);
        entry.instructions.push_back(eir::Instruction::make(
            eir::Opcode::Store, {}, eir::Operand::object(to), eir::Operand::reg(1)));
        entry.instructions.push_back(eir::Instruction::make(eir::Opcode::Return));
        process.blocks.push_back(std::move(entry));
        unit.processes.push_back(std::move(process));
    }
    return true;
}

std::optional<std::string> Specialiser::unitFor(std::uint32_t index,
                                                const std::string& instancePath,
                                                const ValueMap& actuals,
                                                const InterfaceMap& interfaces) {
    const eir::ModuleDefinition& definition = analysed.module(index);
    const std::string& sourceName =
        definition.sourceName.empty() ? definition.name : definition.sourceName;

    const auto report = [&](const eir::SpecialiseFailure& failure) {
        switch (failure.kind) {
        case eir::SpecialiseFailure::Kind::NoValue:
            diagnostics.report(DiagCode::ParameterHasNoValue, failure.location, failure.formal,
                               sourceName, instancePath);
            break;
        case eir::SpecialiseFailure::Kind::UnknownActual:
            diagnostics.report(DiagCode::UnknownParameterActual, failure.location, instancePath,
                               failure.formal, sourceName);
            break;
        case eir::SpecialiseFailure::Kind::UntypedMismatch:
            diagnostics.report(
                DiagCode::UntypedParameterType, failure.location, failure.formal, sourceName,
                instancePath, describeType(failure.givenWidth, failure.givenSigned),
                describeType(failure.expectedWidth, failure.expectedSigned), sourceName);
            break;
        case eir::SpecialiseFailure::Kind::Evaluation:
            diagnostics.report(DiagCode::ParameterEvaluationFailed, failure.location, sourceName,
                               instancePath, describe(failure.evaluation));
            break;
        }
        return std::nullopt;
    };

    auto formals = eir::formalValues(
        definition, actuals,
        [&](std::string_view port, std::string_view name) -> std::optional<eir::ParameterValue> {
            const auto interface = interfaces.find(port);
            if (interface == interfaces.end()) return std::nullopt;
            const auto found = interface->second.parameters.find(name);
            if (found == interface->second.parameters.end()) return std::nullopt;
            return found->second;
        },
        functions);
    if (const auto* failure = std::get_if<eir::SpecialiseFailure>(&formals))
        return report(*failure);
    const auto& values = std::get<std::vector<std::optional<eir::ParameterValue>>>(formals);

    std::vector<eir::NamedValue> nameValues;
    for (std::size_t formalIndex = 0; formalIndex < definition.formals.size(); ++formalIndex) {
        const eir::ParameterFormal& formal = definition.formals[formalIndex];
        if (formal.kind == eir::FormalKind::Parameter) {
            nameValues.push_back(eir::NamedValue{
                .name = formal.name, .value = eir::renderParameterValue(*values[formalIndex])});
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

    auto specialised = eir::specialiseAt(definition, values, functions);
    if (const auto* failure = std::get_if<eir::SpecialiseFailure>(&specialised))
        return report(*failure);
    eir::ModuleDefinition unit = std::move(std::get<eir::ModuleDefinition>(specialised));
    unit.name = name;

    inProgress.push_back(name);
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
    // Whether each instance's definition is generic: a connection to one, or from inside one,
    // may join two widths only specialisation decided.
    std::vector<bool> childGeneric(unit.instances.size(), false);
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
        if (childIndexInDesign.has_value())
            childGeneric[childIndex] = analysed.module(*childIndexInDesign).isGeneric();
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

        const ValueMap childActuals = eir::actualValues(unit, child);
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
        if ((definition.isGeneric() || childGeneric[child]) && !adaptPorts(unit, child))
            return std::nullopt;
    }
    unit.expressions = {};

    // A specialisation is checked as a frontend's unit is: an annotation the frontend got wrong
    // shows up here as a unit that does not verify, not as a simulation that is quietly wrong.
    if (definition.isGeneric() && !eir::verify(unit, nullptr, diagnostics)) return std::nullopt;

    units->addModule(std::move(unit));
    return reference;
}

} // namespace enzan::elab
