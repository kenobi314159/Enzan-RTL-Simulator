// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Link.h"

#include "common/Diagnostics.h"
#include "common/Names.h"
#include "eir/Design.h"
#include "eir/Module.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace enzan::elab {

namespace {

/// The index of `definition`'s port called `name`: exactly, or else in another case when only one
/// port is.
std::optional<std::uint32_t> portNamed(const eir::ModuleDefinition& definition,
                                       std::string_view name) {
    std::optional<std::uint32_t> found;
    bool several = false;
    for (std::uint32_t index = 0; index < definition.ports.size(); ++index) {
        if (definition.ports[index].name == name) return index;
        if (!equalsIgnoringCase(definition.ports[index].name, name)) continue;
        several = found.has_value();
        found = index;
    }
    return several ? std::nullopt : found;
}

} // namespace

bool linkRequests(eir::Design& design, std::span<const eir::UnitRequest> requests,
                  std::span<const std::string> answers, DiagnosticEngine& diagnostics) {
    std::map<std::string_view, std::size_t, std::less<>> byPlaceholder;
    for (std::size_t index = 0; index < requests.size(); ++index)
        byPlaceholder.emplace(requests[index].placeholder, index);
    if (answers.size() != requests.size()) {
        diagnostics.report(
            DiagCode::InternalError, SourceLocation(),
            std::format("{} units were asked for and {} given", requests.size(), answers.size()));
        return false;
    }

    bool linked = true;
    for (std::uint32_t index = 0; index < design.size(); ++index) {
        const auto asks = [&byPlaceholder](const eir::Instance& instance) {
            return byPlaceholder.contains(instance.definitionName);
        };
        if (std::ranges::none_of(design.module(index).instances, asks)) continue;

        eir::ModuleDefinition parent = design.module(index);
        for (eir::Instance& instance : parent.instances) {
            const auto request = byPlaceholder.find(instance.definitionName);
            if (request == byPlaceholder.end()) continue;
            const eir::UnitRequest& asked = requests[request->second];
            const std::string& answer = answers[request->second];
            const eir::ModuleDefinition* definition = design.findModule(answer);
            if (definition == nullptr) {
                diagnostics.report(DiagCode::InternalError, instance.location,
                                   std::format("'{}' was asked for and '{}' given, which the "
                                               "design does not hold",
                                               asked.placeholder, answer));
                linked = false;
                continue;
            }

            for (eir::PortConnection& connection : instance.connections) {
                if (connection.portIndex >= asked.ports.size()) {
                    diagnostics.report(DiagCode::InternalError, instance.location,
                                       std::format("instance '{}' connects port {} of '{}', "
                                                   "which was asked for with {} ports",
                                                   instance.name, connection.portIndex,
                                                   asked.placeholder, asked.ports.size()));
                    linked = false;
                    continue;
                }
                const eir::UnitRequest::PortUse& use = asked.ports[connection.portIndex];
                const std::optional<std::uint32_t> port = portNamed(*definition, use.name);
                if (!port) {
                    diagnostics.report(DiagCode::RequestedPortMissing, instance.location,
                                       asked.unit, use.name, instance.name);
                    linked = false;
                    continue;
                }
                const eir::PortDirection direction = definition->ports[*port].direction;
                if (direction != use.direction) {
                    diagnostics.report(DiagCode::RequestedPortDirection, instance.location,
                                       definition->ports[*port].name, asked.unit,
                                       eir::toString(direction), instance.name,
                                       eir::toString(use.direction));
                    linked = false;
                    continue;
                }
                connection.portIndex = *port;
            }
            instance.definitionName = answer;
        }
        design.addModule(std::move(parent));
    }
    return linked;
}

} // namespace enzan::elab
