// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Elaborator.h"

#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "eir/Module.h"
#include "eir/Process.h"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace enzan::elab {

namespace {

/// Joins a list of names for a diagnostic, in the order given.
template<typename Range>
std::string joinNames(const Range& names) {
    if (names.empty()) return "(none)";

    std::string result;
    for (const std::string_view name : names) {
        if (!result.empty()) result += ", ";
        result += '\'';
        result += name;
        result += '\'';
    }
    return result;
}

/// How a slot is written, so that conflicting drivers can be detected before they
/// produce silently wrong values.
enum class DriverKind : std::uint8_t {
    /// Nothing writes it.
    None,
    /// One or more procedural blocks write it. Several are legal and normal: a
    /// testbench that has `initial clk = 0;` alongside `always #5 clk = ~clk;` has two
    /// procedural writers of one variable, and the last write wins.
    Procedural,
    /// A continuous assignment drives it.
    Continuous,
    /// More than one continuous assignment, or a mixture of kinds. Reported.
    Conflicting,
};

} // namespace

/// Does the work, so that Elaborator itself stays a thin, reusable entry point and
/// all the mutable bookkeeping has a scope that ends.
class Elaborator::Builder {
public:
    Builder(const eir::Design& analysedDesign, DiagnosticEngine& engine)
        : design(analysedDesign), diagnostics(engine) {}

    std::unique_ptr<ElaboratedDesign> run(std::string_view topModuleName) {
        const std::optional<std::uint32_t> topIndex = resolveTop(topModuleName);
        if (!topIndex.has_value()) return nullptr;

        result = std::make_unique<ElaboratedDesign>();
        result->topName = design.module(*topIndex).name;

        // The top instance's path is its module name, so that a design elaborated
        // with `tb` as its top has `/tb/...` paths — which is what a user typing a
        // a hierarchical path like /tb/u_dut/count names.
        const std::uint32_t topInstance =
            createInstance("/" + design.module(*topIndex).name, design.module(*topIndex).name,
                           *topIndex, Instance::NoParent);
        if (!expand(topInstance)) return nullptr;

        collectProcesses();
        if (!checkDrivers()) return nullptr;

        return std::move(result);
    }

private:
    std::optional<std::uint32_t> resolveTop(std::string_view topModuleName) {
        if (!topModuleName.empty()) {
            std::vector<std::string_view> ambiguity;
            const std::optional<std::uint32_t> index =
                design.findModuleIndex(topModuleName, &ambiguity);
            if (!index.has_value() && !ambiguity.empty()) {
                diagnostics.report(DiagCode::AmbiguousModule, SourceLocation(), topModuleName,
                                   joinNames(ambiguity), topModuleName);
                return std::nullopt;
            }
            // A parameterised module is named for its parameter values, so the name a user
            // wrote is its source name. Tried second, so an exact name still wins.
            if (!index.has_value()) {
                const std::vector<std::uint32_t> specialisations =
                    design.findSpecialisations(topModuleName);
                if (specialisations.size() == 1) return specialisations.front();
                if (specialisations.size() > 1) {
                    std::vector<std::string_view> names;
                    names.reserve(specialisations.size());
                    for (const std::uint32_t candidate : specialisations)
                        names.push_back(design.module(candidate).name);
                    diagnostics.report(DiagCode::AmbiguousSpecialisation, SourceLocation(),
                                       topModuleName, joinNames(names));
                    return std::nullopt;
                }
            }

            if (!index.has_value()) {
                std::vector<std::string> names;
                for (const eir::ModuleDefinition& definition : design.modules())
                    names.push_back(definition.name);
                diagnostics.report(DiagCode::TopModuleNotFound, SourceLocation(), topModuleName,
                                   joinNames(names));
                return std::nullopt;
            }
            return index;
        }

        // Choosing a top level is safe only when the choice is forced.
        const std::vector<std::string> candidates = design.candidateTopModules();
        if (candidates.size() == 1) return design.findModuleIndex(candidates.front());

        diagnostics.report(DiagCode::NoTopModule, SourceLocation(), candidates.size(),
                           joinNames(candidates));
        return std::nullopt;
    }

    std::uint32_t createInstance(std::string path, std::string name, std::uint32_t definitionIndex,
                                 std::uint32_t parent) {
        const auto index = static_cast<std::uint32_t>(result->instanceList.size());

        Instance instance;
        instance.path = std::move(path);
        instance.name = std::move(name);
        instance.definitionIndex = definitionIndex;
        instance.parent = parent;
        instance.objectSlots.assign(design.module(definitionIndex).objects.size(), NoSlot);

        result->instancesByPath.emplace(instance.path, index);
        result->instanceList.push_back(std::move(instance));

        if (parent != Instance::NoParent) result->instanceList[parent].children.push_back(index);

        return index;
    }

    /// Allocates a slot for one object of one instance, or reuses an ancestor's.
    std::uint32_t allocateSlot(std::uint32_t instanceIndex, std::uint32_t objectIndex) {
        const Instance& instance = result->instanceList[instanceIndex];
        const eir::ModuleDefinition& definition = design.module(instance.definitionIndex);
        const eir::Object& object = definition.objects[objectIndex];

        const auto slotIndex = static_cast<std::uint32_t>(result->slotList.size());

        Slot slot;
        slot.type = object.type;
        slot.wordOffset = result->arenaSize;
        slot.path = instance.path + '/' + object.name;
        slot.enumIndex = internEnum(definition, object.enumIndex);
        result->arenaSize += object.type.storageWords();

        result->slotList.push_back(std::move(slot));
        return slotIndex;
    }

    /// Copies a definition's enumeration into the elaborated design's table, or finds the
    /// copy already there. Returns `Slot::NoEnum` when the object has no enumeration.
    ///
    /// Copied rather than pointed at because an `ElaboratedDesign` outlives no part of the
    /// analysed one by contract but is handed around on its own, and a table of a handful of
    /// names is cheaper to copy than a lifetime rule is to keep.
    std::uint32_t internEnum(const eir::ModuleDefinition& definition, std::uint32_t enumIndex) {
        if (enumIndex == eir::Object::NoEnum) return Slot::NoEnum;
        if (enumIndex >= definition.enums.size()) return Slot::NoEnum;

        const eir::EnumDefinition& wanted = definition.enums[enumIndex];

        const auto same = [&wanted](const eir::EnumDefinition& existing) {
            if (existing.name != wanted.name) return false;
            if (existing.members.size() != wanted.members.size()) return false;

            return std::ranges::equal(
                existing.members, wanted.members,
                [](const eir::EnumMember& left, const eir::EnumMember& right) {
                    return left.name == right.name && left.value == right.value;
                });
        };

        const auto found = std::ranges::find_if(result->enumList, same);
        if (found != result->enumList.end()) {
            return static_cast<std::uint32_t>(found - result->enumList.begin());
        }

        const auto index = static_cast<std::uint32_t>(result->enumList.size());
        result->enumList.push_back(wanted);
        return index;
    }

    /// Records that `path` names `slotIndex`.
    void nameSlot(const std::string& path, std::uint32_t slotIndex) {
        result->slotsByPath.emplace(path, slotIndex);
    }

    /// Expands one instance: allocates its slots, binds its ports and recurses.
    bool expand(std::uint32_t instanceIndex) {
        const eir::ModuleDefinition& definition =
            design.module(result->instanceList[instanceIndex].definitionIndex);

        // Objects first, so that a child's port can find the parent object it binds
        // to. Ports are collapsed onto the parent's slot when the instance is created
        // below, which is why this loop skips nothing: an object that turns out to be
        // a collapsed port has its slot overwritten before anything reads it.
        for (std::uint32_t objectIndex = 0; objectIndex < definition.objects.size();
             ++objectIndex) {
            if (result->instanceList[instanceIndex].objectSlots[objectIndex] != NoSlot) continue;

            const std::uint32_t slotIndex = allocateSlot(instanceIndex, objectIndex);
            result->instanceList[instanceIndex].objectSlots[objectIndex] = slotIndex;
            nameSlot(result->instanceList[instanceIndex].path + '/'
                         + definition.objects[objectIndex].name,
                     slotIndex);
        }

        return std::ranges::all_of(definition.instances, [&](const eir::Instance& child) {
            return expandChild(instanceIndex, child);
        });
    }

    bool expandChild(std::uint32_t parentIndex, const eir::Instance& child) {
        std::vector<std::string_view> ambiguity;
        const std::optional<std::uint32_t> childDefinition =
            design.findModuleIndex(child.definitionName, &ambiguity);
        if (!childDefinition.has_value() && !ambiguity.empty()) {
            diagnostics.report(DiagCode::AmbiguousModule, child.location, child.definitionName,
                               joinNames(ambiguity), child.definitionName);
            return false;
        }
        if (!childDefinition.has_value()) {
            diagnostics.report(DiagCode::UnresolvedInstance, child.location, child.name,
                               child.definitionName);
            return false;
        }

        // Recursion is checked along the current path rather than globally: a module
        // may legitimately appear many times in a hierarchy, just never inside itself.
        if (activeDefinitions.contains(*childDefinition)) {
            diagnostics.report(DiagCode::RecursiveInstantiation, child.location,
                               child.definitionName, result->instanceList[parentIndex].path);
            return false;
        }

        const std::string path = result->instanceList[parentIndex].path + '/' + child.name;
        const std::uint32_t childIndex =
            createInstance(path, child.name, *childDefinition, parentIndex);

        if (!bindPorts(parentIndex, childIndex, child)) return false;

        activeDefinitions.insert(*childDefinition);
        const bool expanded = expand(childIndex);
        activeDefinitions.erase(*childDefinition);
        return expanded;
    }

    /// Collapses each connected port onto the parent object's slot.
    ///
    /// Collapsing rather than generating an implicit continuous assignment per port:
    /// a connection between two objects of the same width is the same storage under
    /// two names, which is what commercial tools do and what makes a port cost
    /// nothing at run time. The alternative would add a process and a delta cycle to
    /// every port in the design.
    bool bindPorts(std::uint32_t parentIndex, std::uint32_t childIndex,
                   const eir::Instance& child) {
        const eir::ModuleDefinition& childDefinition =
            design.module(result->instanceList[childIndex].definitionIndex);
        const Instance& parent = result->instanceList[parentIndex];

        return std::ranges::all_of(child.connections, [&](const eir::PortConnection& connection) {
            ENZAN_ASSERT(connection.portIndex < childDefinition.ports.size(),
                         "the verifier rejects an out-of-range port index");
            const eir::Port& port = childDefinition.ports[connection.portIndex];

            if (!connection.connected()) {
                // Left to read as X rather than tied off. A silently tied-off input is
                // how a missing connection turns into a design that appears to work.
                diagnostics.report(DiagCode::UnconnectedPort, port.location, port.name,
                                   result->instanceList[childIndex].path);
                return true;
            }

            const std::uint32_t parentSlot = parent.objectSlots[connection.objectIndex];
            ENZAN_ASSERT(parentSlot != NoSlot, "a parent's objects are allocated before its "
                                               "children are expanded");

            const eir::Type portType = childDefinition.objects[port.objectIndex].type;
            const eir::Type parentType = result->slotList[parentSlot].type;
            if (portType.bitWidth() != parentType.bitWidth()) {
                diagnostics.report(DiagCode::PortWidthMismatch, port.location, port.name,
                                   child.definitionName, portType.bitWidth(),
                                   parentType.bitWidth());
                return false;
            }

            result->instanceList[childIndex].objectSlots[port.objectIndex] = parentSlot;
            nameSlot(result->instanceList[childIndex].path + '/'
                         + childDefinition.objects[port.objectIndex].name,
                     parentSlot);
            return true;
        });
    }

    void collectProcesses() {
        for (std::uint32_t instanceIndex = 0; instanceIndex < result->instanceList.size();
             ++instanceIndex) {
            const eir::ModuleDefinition& definition =
                design.module(result->instanceList[instanceIndex].definitionIndex);
            for (std::uint32_t processIndex = 0; processIndex < definition.processes.size();
                 ++processIndex) {
                result->processList.push_back(
                    ProcessInstance{.instanceIndex = instanceIndex, .processIndex = processIndex});
            }
        }
    }

    /// Reports slots that would need driver resolution, which is an M5 feature.
    ///
    /// Catching this here rather than letting the runtime take the last write is the
    /// difference between a clear error and a design that simulates to a plausible
    /// wrong answer — the failure mode section 1.2 weighs most heavily.
    bool checkDrivers() {
        std::vector<DriverKind> drivers(result->slotList.size(), DriverKind::None);

        for (const ProcessInstance& processInstance : result->processList) {
            const Instance& instance = result->instanceList[processInstance.instanceIndex];
            const eir::ModuleDefinition& definition = design.module(instance.definitionIndex);
            const eir::Process& process = definition.processes[processInstance.processIndex];
            const bool continuous = process.kind == eir::ProcessKind::ContinuousAssign;

            for (const std::uint32_t objectIndex : process.writeSet) {
                const std::uint32_t slotIndex = instance.objectSlots[objectIndex];
                DriverKind& kind = drivers[slotIndex];

                if (kind == DriverKind::None) {
                    kind = continuous ? DriverKind::Continuous : DriverKind::Procedural;
                } else if (continuous || kind == DriverKind::Continuous) {
                    // Two continuous drivers need resolution; a continuous driver
                    // fighting a procedural one is a design error in any case.
                    kind = DriverKind::Conflicting;
                }
            }
        }

        bool ok = true;
        for (std::uint32_t slotIndex = 0; slotIndex < drivers.size(); ++slotIndex) {
            if (drivers[slotIndex] != DriverKind::Conflicting) continue;

            diagnostics.report(DiagCode::MultipleContinuousDrivers, SourceLocation(),
                               result->slotList[slotIndex].path);
            ok = false;
        }
        return ok;
    }

    static constexpr std::uint32_t NoSlot = 0xFFFF'FFFFU;

    const eir::Design& design;
    DiagnosticEngine& diagnostics;
    std::unique_ptr<ElaboratedDesign> result;

    /// Definitions on the current path from the top, for recursion detection. An
    /// ordered set, so that the diagnostic it feeds is reproducible.
    std::set<std::uint32_t> activeDefinitions;
};

Elaborator::Elaborator(const eir::Design& analysedDesign, DiagnosticEngine& engine) noexcept
    : design(analysedDesign), diagnostics(engine) {}

std::unique_ptr<ElaboratedDesign> Elaborator::elaborate(std::string_view topModuleName) {
    Builder builder(design, diagnostics);
    return builder.run(topModuleName);
}

} // namespace enzan::elab
