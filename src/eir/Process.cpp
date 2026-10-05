// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Process.h"

#include <algorithm>

namespace enzan::eir {

void normaliseWriteSet(std::vector<WriteExtent>& extents) {
    std::ranges::sort(extents, [](const WriteExtent& left, const WriteExtent& right) {
        if (left.objectIndex != right.objectIndex) return left.objectIndex < right.objectIndex;
        return left.lowBit < right.lowBit;
    });

    std::vector<WriteExtent> merged;
    merged.reserve(extents.size());
    for (const WriteExtent& extent : extents) {
        if (!merged.empty() && merged.back().objectIndex == extent.objectIndex
            && extent.lowBit <= merged.back().endBit()) {
            WriteExtent& last = merged.back();
            last.width = std::max(last.endBit(), extent.endBit()) - last.lowBit;
            continue;
        }
        merged.push_back(extent);
    }
    extents = std::move(merged);
}

bool writesObject(const std::vector<WriteExtent>& writeSet, std::uint32_t objectIndex) noexcept {
    const auto found =
        std::ranges::lower_bound(writeSet, objectIndex, {}, &WriteExtent::objectIndex);
    return found != writeSet.end() && found->objectIndex == objectIndex;
}

std::vector<std::uint32_t> writtenObjects(const std::vector<WriteExtent>& writeSet) {
    std::vector<std::uint32_t> objects;
    for (const WriteExtent& extent : writeSet) {
        if (objects.empty() || objects.back() != extent.objectIndex)
            objects.push_back(extent.objectIndex);
    }
    return objects;
}

bool isSystemFunction(SysTaskKind kind) noexcept {
    return kind == SysTaskKind::Random || kind == SysTaskKind::URandom
           || kind == SysTaskKind::URandomRange;
}

std::string_view toString(SysTaskKind kind) noexcept {
    switch (kind) {
    case SysTaskKind::Display: return "$display";
    case SysTaskKind::Write: return "$write";
    case SysTaskKind::Finish: return "$finish";
    case SysTaskKind::Fatal: return "$fatal";
    case SysTaskKind::Error: return "$error";
    case SysTaskKind::Warning: return "$warning";
    case SysTaskKind::Info: return "$info";
    case SysTaskKind::DumpFile: return "$dumpfile";
    case SysTaskKind::DumpVars: return "$dumpvars";
    case SysTaskKind::Random: return "$random";
    case SysTaskKind::URandom: return "$urandom";
    case SysTaskKind::URandomRange: return "$urandom_range";
    }
    return "$unknown";
}

std::string_view toString(ProcessKind kind) noexcept {
    switch (kind) {
    case ProcessKind::Initial: return "initial";
    case ProcessKind::Final: return "final";
    case ProcessKind::Always: return "always";
    case ProcessKind::AlwaysComb: return "always_comb";
    case ProcessKind::AlwaysLatch: return "always_latch";
    case ProcessKind::AlwaysFf: return "always_ff";
    case ProcessKind::ContinuousAssign: return "assign";
    }
    return "always";
}

bool isRepeating(ProcessKind kind) noexcept {
    return kind != ProcessKind::Initial && kind != ProcessKind::Final;
}

bool startsAtTimeZero(const Process& process) noexcept {
    switch (process.kind) {
    case ProcessKind::Initial:
    case ProcessKind::AlwaysComb:
    case ProcessKind::AlwaysLatch:
    case ProcessKind::ContinuousAssign: return true;

    case ProcessKind::Always:
        // A bare `always` loop — the clock generator — has nothing to wait for
        // and must start. One with an event control blocks on it first.
        return process.sensitivity.empty();

    case ProcessKind::AlwaysFf:
    case ProcessKind::Final: return false;
    }
    return false;
}

} // namespace enzan::eir
