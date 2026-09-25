// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/ElaboratedDesign.h"

namespace enzan::elab {

std::optional<std::uint32_t> ElaboratedDesign::findSlot(std::string_view path) const {
    const auto found = slotsByPath.find(path);
    return found == slotsByPath.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::uint32_t> ElaboratedDesign::findInstance(std::string_view path) const {
    const auto found = instancesByPath.find(path);
    return found == instancesByPath.end() ? std::nullopt : std::optional(found->second);
}

std::vector<std::string> ElaboratedDesign::slotNames(std::uint32_t slotIndex) const {
    // A linear pass over the path index rather than a reverse map. The number of
    // names is small, this is not on a hot path, and iterating the ordered map keeps
    // the result sorted for free.
    std::vector<std::string> names;
    for (const auto& [path, index] : slotsByPath) {
        if (index == slotIndex) names.push_back(path);
    }
    return names;
}

} // namespace enzan::elab
