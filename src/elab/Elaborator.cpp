// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Elaborator.h"
#include "elab/JoinNames.h"
#include "elab/Specialiser.h"

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

/// One process's write of a range of one slot, for finding where drivers overlap.
struct DriverRange {
    std::uint32_t lowBit = 0;
    std::uint32_t width = 0;

    /// Index into the elaborated design's process list.
    std::uint32_t process = 0;

    /// True for a continuous assignment, which drives its bits; false for a procedural
    /// block, which only writes them.
    bool continuous = false;

    [[nodiscard]] std::uint32_t endBit() const noexcept { return lowBit + width; }
};

/// Names the overlapping bits of a slot for a diagnostic.
///
/// Counted from bit 0 at the least significant end, which is how every other part of Enzan
/// numbers a signal's bits: the declared range is not kept past the frontend.
std::string describeBits(std::uint32_t lowBit, std::uint32_t endBit, std::uint32_t slotWidth) {
    if (slotWidth == 1) return "its only bit";
    if (endBit - lowBit == 1) return std::format("bit {}", lowBit);
    return std::format("bits [{}:{}]", endBit - 1, lowBit);
}

} // namespace

/// Does the work, so that Elaborator itself stays a thin, reusable entry point and
/// all the mutable bookkeeping has a scope that ends.
class Elaborator::Builder {
public:
    Builder(const eir::Design& analysedDesign, DiagnosticEngine& engine,
            eir::ConstantFunctions* runner)
        : analysed(analysedDesign), diagnostics(engine), functions(runner) {}

    std::unique_ptr<ElaboratedDesign> run(std::string_view topModuleName) {
        const std::optional<std::uint32_t> analysedTop = resolveTop(topModuleName);
        if (!analysedTop.has_value()) return nullptr;

        // Every generic unit under the top becomes the specialisations its instances need, and
        // the rest of elaboration sees only those (DESIGN_GUIDE.md section 18, M5.1).
        Specialiser specialiser(analysed, diagnostics, functions);
        std::optional<Specialiser::Result> specialised = specialiser.specialise(*analysedTop);
        if (!specialised.has_value()) return nullptr;
        const std::uint32_t topIndex = specialised->top;

        // The top instance is named for the module as the source names it, so that a design
        // elaborated with `tb` as its top has `/tb/...` paths — which is what a user typing a
        // hierarchical path like /tb/u_dut/count names. The source's name rather than the
        // definition's, because a parameterised top's definition is named for its values
        // (section 8.2), and `/counter$WIDTH_8/count` is a path nobody would write. The design
        // reports its top by the same name, since that is the name a front end shows.
        const eir::ModuleDefinition& top = specialised->units->module(topIndex);
        const std::string& topName = top.sourceName.empty() ? top.name : top.sourceName;

        result = std::make_unique<ElaboratedDesign>();
        result->unitDesign = std::move(specialised->units);
        design = result->unitDesign.get();
        result->topName = topName;
        const std::uint32_t topInstance =
            createInstance("/" + topName, topName, topIndex, Instance::NoParent);
        if (!expand(topInstance)) return nullptr;
        if (!instantiatePackages()) return nullptr;
        if (!bindHierarchicalReferences()) return nullptr;

        collectProcesses();
        if (!checkDrivers()) return nullptr;

        return std::move(result);
    }

private:
    std::optional<std::uint32_t> resolveTop(std::string_view topModuleName) {
        if (!topModuleName.empty()) {
            std::vector<std::string_view> ambiguity;
            const std::optional<std::uint32_t> index =
                analysed.findModuleIndex(topModuleName, &ambiguity);
            if (!index.has_value() && !ambiguity.empty()) {
                diagnostics.report(DiagCode::AmbiguousModule, SourceLocation(), topModuleName,
                                   joinNames(ambiguity), topModuleName);
                return std::nullopt;
            }
            // A parameterised module is named for its parameter values, so the name a user
            // wrote is its source name. Tried second, so an exact name still wins.
            if (!index.has_value()) {
                const std::vector<std::uint32_t> specialisations =
                    analysed.findSpecialisations(topModuleName);
                if (specialisations.size() == 1) return specialisations.front();
                if (specialisations.size() > 1) {
                    std::vector<std::string_view> names;
                    names.reserve(specialisations.size());
                    for (const std::uint32_t candidate : specialisations)
                        names.push_back(analysed.module(candidate).name);
                    diagnostics.report(DiagCode::AmbiguousSpecialisation, SourceLocation(),
                                       topModuleName, joinNames(names));
                    return std::nullopt;
                }
            }

            if (!index.has_value()) {
                std::vector<std::string> names;
                for (const eir::ModuleDefinition& definition : analysed.modules())
                    names.push_back(definition.name);
                diagnostics.report(DiagCode::TopModuleNotFound, SourceLocation(), topModuleName,
                                   joinNames(names));
                return std::nullopt;
            }
            return index;
        }

        // Choosing a top level is safe only when the choice is forced.
        const std::vector<std::string> candidates = analysed.candidateTopModules();
        if (candidates.size() == 1) return analysed.findModuleIndex(candidates.front());

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
        instance.objectSlots.assign(design->module(definitionIndex).objects.size(), NoSlot);

        result->instancesByPath.emplace(instance.path, index);
        result->instanceList.push_back(std::move(instance));

        if (parent != Instance::NoParent) result->instanceList[parent].children.push_back(index);

        return index;
    }

    /// Allocates a slot for one object of one instance, or reuses an ancestor's.
    std::uint32_t allocateSlot(std::uint32_t instanceIndex, std::uint32_t objectIndex) {
        const Instance& instance = result->instanceList[instanceIndex];
        const eir::ModuleDefinition& definition = design->module(instance.definitionIndex);
        const eir::Object& object = definition.objects[objectIndex];

        const auto slotIndex = static_cast<std::uint32_t>(result->slotList.size());

        Slot slot;
        slot.type = object.type;
        slot.kind = object.kind;
        slot.netKind = object.netKind;
        slot.unpackedDimensions = object.unpackedDimensions;
        slot.elementWidth = object.elementWidth;
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
            design->module(result->instanceList[instanceIndex].definitionIndex);

        // Objects first, so that a child's port can find the parent object it binds
        // to. Ports are collapsed onto the parent's slot when the instance is created
        // below, which is why this loop skips nothing: an object that turns out to be
        // a collapsed port has its slot overwritten before anything reads it.
        for (std::uint32_t objectIndex = 0; objectIndex < definition.objects.size();
             ++objectIndex) {
            if (result->instanceList[instanceIndex].objectSlots[objectIndex] != NoSlot) continue;
            // A hierarchical reference has no storage of its own; it is bound once the whole
            // tree exists, since what it names may be in a part not yet expanded.
            if (!definition.objects[objectIndex].hierarchicalPath.empty()) continue;

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
            design->findModuleIndex(child.definitionName, &ambiguity);
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
        if (!bindInterfaces(parentIndex, childIndex, child)) return false;

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
    /// every port in the design->
    bool bindPorts(std::uint32_t parentIndex, std::uint32_t childIndex,
                   const eir::Instance& child) {
        const eir::ModuleDefinition& childDefinition =
            design->module(result->instanceList[childIndex].definitionIndex);
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

    /// Resolves what each of a child's interface ports reaches: an interface instance in the
    /// parent, or whatever the parent's own interface port of that name reaches.
    bool bindInterfaces(std::uint32_t parentIndex, std::uint32_t childIndex,
                        const eir::Instance& child) {
        for (const eir::InterfaceBinding& binding : child.interfaceBindings) {
            std::string target;
            if (binding.target.starts_with('@')) {
                // `@port`, or `@port[2]` for an element of an interface array the parent
                // received through an array port.
                const std::string_view through = std::string_view(binding.target).substr(1);
                const std::size_t index = through.find('[');
                const auto& bound = result->instanceList[parentIndex].interfaces;
                const auto found = bound.find(through.substr(0, index));
                if (found == bound.end()) {
                    diagnostics.report(DiagCode::UnresolvedHierarchicalReference, child.location,
                                       binding.target, result->instanceList[parentIndex].path);
                    return false;
                }
                target = found->second;
                if (index != std::string_view::npos) target += through.substr(index);
            } else {
                target = result->instanceList[parentIndex].path + '/' + binding.target;
            }
            result->instanceList[childIndex].interfaces.insert_or_assign(binding.port, target);
        }
        return true;
    }

    /// Binds every hierarchical reference to the slot of the object it names.
    /// Gives each package whose variables something refers to its one instance, at the root
    /// beside the top level, as `/<package>` (eir::ModuleDefinition::isPackage). A reference to
    /// a package variable is the absolute path `/<package>/<variable>`.
    bool instantiatePackages() {
        std::vector<std::string> packages;
        for (const Instance& instance : result->instanceList) {
            for (const eir::Object& object : design->module(instance.definitionIndex).objects) {
                const std::string_view reference = object.hierarchicalPath;
                if (!reference.starts_with('/')) continue;
                const std::string name(reference.substr(1, reference.find('/', 1) - 1));
                if (std::ranges::find(packages, name) == packages.end()) packages.push_back(name);
            }
        }

        return std::ranges::all_of(packages, [this](const std::string& name) {
            // A package that is missing leaves its references unresolved, which says so.
            const std::optional<std::uint32_t> definition = design->findModuleIndex(name + "::");
            if (!definition.has_value()) return true;
            if (name == result->topName) {
                diagnostics.report(DiagCode::PackageNamedAsTop,
                                   design->module(*definition).location, name, name);
                return false;
            }
            return expand(createInstance("/" + name, name, *definition, Instance::NoParent));
        });
    }

    bool bindHierarchicalReferences() {
        bool ok = true;
        for (Instance& instance : result->instanceList) {
            const eir::ModuleDefinition& definition = design->module(instance.definitionIndex);
            for (std::uint32_t objectIndex = 0; objectIndex < definition.objects.size();
                 ++objectIndex) {
                const std::string& reference = definition.objects[objectIndex].hierarchicalPath;
                if (reference.empty()) continue;

                // `@port/` starts from the interface that port reaches, and each leading `../`
                // climbs one instance from the one holding the reference.
                std::string base = instance.path;
                std::string_view rest = reference;
                if (rest.starts_with('/')) {
                    // An absolute path, to a package's variable: from the root.
                    const std::size_t slash = rest.find('/', 1);
                    base = std::string(rest.substr(0, slash));
                    rest = slash == std::string_view::npos ? std::string_view()
                                                           : rest.substr(slash + 1);
                } else if (rest.starts_with('@')) {
                    // The port's name runs to an index, `@in[2]/data`, or to the member.
                    const std::size_t slash = rest.find('/');
                    const std::string_view through = rest.substr(1, slash - 1);
                    const std::size_t index = through.find('[');
                    const auto found = instance.interfaces.find(through.substr(0, index));
                    base = found != instance.interfaces.end() ? found->second : std::string();
                    if (!base.empty() && index != std::string_view::npos)
                        base += through.substr(index);
                    rest = slash == std::string_view::npos ? std::string_view()
                                                           : rest.substr(slash + 1);
                }
                while (rest.starts_with("../")) {
                    rest.remove_prefix(3);
                    const std::size_t slash = base.rfind('/');
                    base = slash == 0 || slash == std::string::npos ? std::string()
                                                                    : base.substr(0, slash);
                }
                const std::string path = base + '/' + std::string(rest);

                const auto found = result->slotsByPath.find(path);
                if (base.empty() || found == result->slotsByPath.end()) {
                    diagnostics.report(DiagCode::UnresolvedHierarchicalReference,
                                       definition.objects[objectIndex].location, reference,
                                       instance.path);
                    ok = false;
                    continue;
                }
                instance.objectSlots[objectIndex] = found->second;
            }
        }
        return ok;
    }

    void collectProcesses() {
        for (std::uint32_t instanceIndex = 0; instanceIndex < result->instanceList.size();
             ++instanceIndex) {
            const eir::ModuleDefinition& definition =
                design->module(result->instanceList[instanceIndex].definitionIndex);
            for (std::uint32_t processIndex = 0; processIndex < definition.processes.size();
                 ++processIndex) {
                result->processList.push_back(
                    ProcessInstance{.instanceIndex = instanceIndex, .processIndex = processIndex});
            }
        }
    }

    /// Finds bits with more than one driver, reports those the language does not allow, and
    /// builds the drivers and segments of the signals that resolve (DESIGN_GUIDE.md section
    /// 3.7).
    ///
    /// Overlap is what counts, not sharing a signal: `N` instances each driving one bit of an
    /// array are `N` drivers of disjoint ranges and no conflict at all. Several procedural
    /// writers of one bit are legal too — `initial clk = 0;` beside `always #5 clk = ~clk;`
    /// is in almost every testbench — and the last write wins. What is refused is a bit of a
    /// variable or a `uwire` with two continuous drivers, or with a continuous driver and a
    /// procedural writer. Overlapping drivers of any other net are legal and resolved.
    ///
    /// One diagnostic per slot, for the overlap at its lowest bit: the first conflict names
    /// the signal and both drivers, and a list of every pair on a wide bus would bury it.
    bool checkDrivers() {
        std::vector<std::vector<DriverRange>> rangesBySlot(result->slotList.size());

        for (std::uint32_t processIndex = 0; processIndex < result->processList.size();
             ++processIndex) {
            const ProcessInstance& processInstance = result->processList[processIndex];
            const Instance& instance = result->instanceList[processInstance.instanceIndex];
            const eir::Process& process = processOf(processInstance);
            const std::vector<eir::Object>& objects =
                design->module(instance.definitionIndex).objects;
            const bool continuous = process.kind == eir::ProcessKind::ContinuousAssign;

            // A subroutine's static variable is written by every call of it, and a call inlined
            // into a continuous assignment no more drives it than one in an `always` does.
            for (const eir::WriteExtent& extent : process.writeSet) {
                rangesBySlot[instance.objectSlots[extent.objectIndex]].push_back(DriverRange{
                    .lowBit = extent.lowBit,
                    .width = extent.width,
                    .process = processIndex,
                    .continuous = continuous && !objects[extent.objectIndex].subroutineStatic});
            }
        }

        bool ok = true;
        std::vector<bool> resolved(rangesBySlot.size(), false);
        for (std::uint32_t slotIndex = 0; slotIndex < rangesBySlot.size(); ++slotIndex) {
            std::vector<DriverRange>& ranges = rangesBySlot[slotIndex];

            // Stable, so that ranges starting at one bit keep process order and the pair a
            // diagnostic names is the same on every run (section 3.8).
            std::ranges::stable_sort(ranges, {}, &DriverRange::lowBit);
            if (!checkSlotDrivers(slotIndex, ranges)) {
                ok = false;
                continue;
            }
            resolved[slotIndex] = resolves(slotIndex, ranges);
            recordDrivenBits(slotIndex, ranges);
        }
        if (!ok) return false;

        buildDrivers(resolved);
        return true;
    }

    /// Reports the first conflicting overlap among `ranges`, which are ordered by low bit,
    /// and returns false if there was one.
    bool checkSlotDrivers(std::uint32_t slotIndex, const std::vector<DriverRange>& ranges) {
        const Slot& slot = result->slotList[slotIndex];
        const bool singleDriver =
            slot.kind == eir::ObjectKind::Variable || slot.netKind == eir::NetKind::UWire;

        for (std::size_t later = 1; later < ranges.size(); ++later) {
            for (std::size_t earlier = 0; earlier < later; ++earlier) {
                const DriverRange& first = ranges[earlier];
                const DriverRange& second = ranges[later];
                if (first.endBit() <= second.lowBit) continue;
                if (!first.continuous && !second.continuous) continue;
                if (first.continuous && second.continuous && !singleDriver) continue;

                const std::string bits = describeBits(
                    second.lowBit, std::min(first.endBit(), second.endBit()), slot.type.bitWidth());
                const eir::Process& secondProcess = processOf(result->processList[second.process]);

                if (first.continuous != second.continuous) {
                    const DriverRange& driver = first.continuous ? first : second;
                    const DriverRange& writer = first.continuous ? second : first;
                    diagnostics.report(DiagCode::MixedDriverKinds, secondProcess.location,
                                       slot.path, bits, describeProcess(driver.process),
                                       describeProcess(writer.process));
                } else {
                    const std::string_view kind =
                        slot.kind == eir::ObjectKind::Variable ? "variable" : "uwire net";
                    diagnostics.report(DiagCode::MultipleContinuousDrivers, secondProcess.location,
                                       kind, slot.path, bits, describeProcess(first.process),
                                       describeProcess(second.process), kind);
                }
                return false;
            }
        }
        return true;
    }

    /// Records which bits of a net its continuous drivers cover.
    void recordDrivenBits(std::uint32_t slotIndex, const std::vector<DriverRange>& ranges) {
        Slot& slot = result->slotList[slotIndex];
        if (slot.kind != eir::ObjectKind::Net) return;
        for (const DriverRange& range : ranges) {
            if (range.continuous)
                slot.drivenBits.push_back(eir::WriteExtent{
                    .objectIndex = 0, .lowBit = range.lowBit, .width = range.width});
        }
        eir::normaliseWriteSet(slot.drivenBits);
    }

    /// True when a slot's drivers have to be resolved rather than written straight into it: a
    /// net with a bit driven twice, or a pulled or supply net with any driver at all, since a
    /// `tri0` driven to `z` reads 0.
    [[nodiscard]] bool resolves(std::uint32_t slotIndex,
                                const std::vector<DriverRange>& ranges) const {
        const Slot& slot = result->slotList[slotIndex];
        if (slot.kind != eir::ObjectKind::Net) return false;

        std::uint32_t reached = 0;
        bool anyDriver = false;
        for (const DriverRange& range : ranges) {
            if (!range.continuous) continue;
            if (anyDriver && range.lowBit < reached) return true;
            reached = std::max(reached, range.endBit());
            anyDriver = true;
        }
        return anyDriver && eir::resolvesSingleDrivers(slot.netKind);
    }

    /// Gives every continuous write of a resolved slot a driver with its own value in the
    /// arena, and splits each resolved slot into segments.
    ///
    /// Drivers are made in process order, and within a process in write-set order, so that a
    /// process's drivers through one object are contiguous: that run is what a store is routed
    /// to (section 3.7).
    void buildDrivers(const std::vector<bool>& resolved) {
        std::vector<std::vector<std::uint32_t>> driversBySlot(result->slotList.size());

        for (std::uint32_t processIndex = 0; processIndex < result->processList.size();
             ++processIndex) {
            const ProcessInstance& processInstance = result->processList[processIndex];
            const eir::Process& process = processOf(processInstance);
            if (process.kind != eir::ProcessKind::ContinuousAssign) continue;

            const Instance& instance = result->instanceList[processInstance.instanceIndex];
            for (const eir::WriteExtent& extent : process.writeSet) {
                const std::uint32_t slotIndex = instance.objectSlots[extent.objectIndex];
                if (!resolved[slotIndex]) continue;

                driversBySlot[slotIndex].push_back(
                    static_cast<std::uint32_t>(result->driverList.size()));
                result->driverList.push_back(Driver{.slot = slotIndex,
                                                    .lowBit = extent.lowBit,
                                                    .width = extent.width,
                                                    .process = processIndex,
                                                    .objectIndex = extent.objectIndex,
                                                    .wordOffset = result->arenaSize});
                result->arenaSize += eir::storageWordsFor(extent.width);
                result->widestDriverBits = std::max(result->widestDriverBits, extent.width);
            }
        }

        for (std::uint32_t slotIndex = 0; slotIndex < driversBySlot.size(); ++slotIndex) {
            if (!driversBySlot[slotIndex].empty())
                buildSegments(slotIndex, driversBySlot[slotIndex]);
        }
    }

    /// Splits a resolved slot into maximal runs of bits with one set of drivers, and tells each
    /// driver which of them it covers.
    ///
    /// A bit no driver covers belongs to no segment: nothing ever writes it, so it keeps the
    /// undriven value it started with.
    void buildSegments(std::uint32_t slotIndex, const std::vector<std::uint32_t>& drivers) {
        std::vector<std::uint32_t> boundaries;
        for (const std::uint32_t driverIndex : drivers) {
            boundaries.push_back(result->driverList[driverIndex].lowBit);
            boundaries.push_back(result->driverList[driverIndex].endBit());
        }
        std::ranges::sort(boundaries);
        boundaries.erase(std::ranges::unique(boundaries).begin(), boundaries.end());

        const auto firstSegment = static_cast<std::uint32_t>(result->segmentList.size());
        std::vector<std::uint32_t> previousSet;
        for (std::size_t index = 0; index + 1 < boundaries.size(); ++index) {
            const std::uint32_t low = boundaries[index];
            const std::uint32_t end = boundaries[index + 1];

            std::vector<std::uint32_t> covering;
            for (const std::uint32_t driverIndex : drivers) {
                const Driver& driver = result->driverList[driverIndex];
                if (driver.lowBit <= low && end <= driver.endBit()) covering.push_back(driverIndex);
            }
            if (covering.empty()) {
                previousSet.clear();
                continue;
            }

            if (covering == previousSet) {
                result->segmentList.back().width += end - low;
                continue;
            }

            result->segmentList.push_back(
                Segment{.slot = slotIndex,
                        .lowBit = low,
                        .width = end - low,
                        .firstDriver = static_cast<std::uint32_t>(result->segmentDriverList.size()),
                        .driverCount = static_cast<std::uint32_t>(covering.size()),
                        .netKind = result->slotList[slotIndex].netKind});
            result->segmentDriverList.insert(result->segmentDriverList.end(), covering.begin(),
                                             covering.end());
            previousSet = std::move(covering);
        }

        for (const std::uint32_t driverIndex : drivers) {
            Driver& driver = result->driverList[driverIndex];
            for (auto segment = firstSegment; segment < result->segmentList.size(); ++segment) {
                const Segment& candidate = result->segmentList[segment];
                if (candidate.lowBit + candidate.width <= driver.lowBit) continue;
                if (candidate.lowBit >= driver.endBit()) break;
                if (driver.segmentCount == 0) driver.firstSegment = segment;
                ++driver.segmentCount;
            }
        }
    }

    [[nodiscard]] const eir::Process& processOf(const ProcessInstance& processInstance) const {
        const Instance& instance = result->instanceList[processInstance.instanceIndex];
        return design->module(instance.definitionIndex).processes[processInstance.processIndex];
    }

    /// A process as a diagnostic names it: what it is and where, and the instance it runs in,
    /// since every instance of a module shares the one written in its source.
    [[nodiscard]] std::string describeProcess(std::uint32_t processIndex) const {
        const ProcessInstance& processInstance = result->processList[processIndex];
        return std::format("{} in {}", processOf(processInstance).name,
                           result->instanceList[processInstance.instanceIndex].path);
    }

    static constexpr std::uint32_t NoSlot = 0xFFFF'FFFFU;

    /// What analysis produced, generic units included; only the top is resolved against it.
    const eir::Design& analysed;
    /// The concrete units under the top, which the result owns and everything else here reads.
    const eir::Design* design = nullptr;
    DiagnosticEngine& diagnostics;
    std::unique_ptr<ElaboratedDesign> result;

    /// Definitions on the current path from the top, for recursion detection. An
    /// ordered set, so that the diagnostic it feeds is reproducible.
    std::set<std::uint32_t> activeDefinitions;
    eir::ConstantFunctions* functions;
};

Elaborator::Elaborator(const eir::Design& analysedDesign, DiagnosticEngine& engine,
                       eir::ConstantFunctions* runner) noexcept
    : design(analysedDesign), diagnostics(engine), functions(runner) {}

std::unique_ptr<ElaboratedDesign> Elaborator::elaborate(std::string_view topModuleName) {
    Builder builder(design, diagnostics, functions);
    return builder.run(topModuleName);
}

} // namespace enzan::elab
