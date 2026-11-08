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
    switch (kind) {
    case SysTaskKind::Random:
    case SysTaskKind::URandom:
    case SysTaskKind::URandomRange:
    case SysTaskKind::FileOpen:
    case SysTaskKind::FileGetCharacter:
    case SysTaskKind::LineWrite:
    case SysTaskKind::LineWriteLine:
    case SysTaskKind::LineReadLine:
    case SysTaskKind::LineReadCharacter:
    case SysTaskKind::LineReadString:
    case SysTaskKind::LineLength:
    case SysTaskKind::LineElement:
    case SysTaskKind::LineSlice:
    case SysTaskKind::FileEnd:
    case SysTaskKind::LineAppend:
    case SysTaskKind::FileReadValue:
    case SysTaskKind::StringNew:
    case SysTaskKind::StringBounds:
    case SysTaskKind::StringSlice:
    case SysTaskKind::StringFromArray:
    case SysTaskKind::StringIntoArray:
    case SysTaskKind::StringCompare:
    case SysTaskKind::StringRead:
    case SysTaskKind::StringMark: return true;
    default: return false;
    }
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
    case SysTaskKind::FileDisplay: return "$fdisplay";
    case SysTaskKind::FileWrite: return "$fwrite";
    case SysTaskKind::FileClose: return "$fclose";
    case SysTaskKind::FileFlush: return "$fflush";
    case SysTaskKind::ReadMemHex: return "$readmemh";
    case SysTaskKind::ReadMemBinary: return "$readmemb";
    case SysTaskKind::Report: return "report";
    case SysTaskKind::FileOpen: return "$fopen";
    case SysTaskKind::FileGetCharacter: return "$fgetc";
    case SysTaskKind::LineDeallocate: return "deallocate";
    // VHDL names each by the subprogram the design calls, which several share.
    case SysTaskKind::LineWrite:
    case SysTaskKind::LineAppend:
    case SysTaskKind::FileWriteValue: return "write";
    case SysTaskKind::LineReadCharacter:
    case SysTaskKind::LineReadString:
    case SysTaskKind::StringRead:
    case SysTaskKind::FileReadValue: return "read";
    case SysTaskKind::LineWriteLine: return "writeline";
    case SysTaskKind::LineReadLine: return "readline";
    case SysTaskKind::LineLength: return "'length";
    case SysTaskKind::LineElement: return "an element of a line";
    case SysTaskKind::LineSlice: return "a slice of a line";
    case SysTaskKind::FileEnd: return "endfile";
    case SysTaskKind::StringNew: return "a string of run-time length";
    case SysTaskKind::StringBounds: return "the bounds of a string";
    case SysTaskKind::StringSlice: return "a part of a string";
    case SysTaskKind::StringSplice: return "an assignment to part of a string";
    case SysTaskKind::StringFromArray: return "a slice of run-time length";
    case SysTaskKind::StringIntoArray: return "an assignment to a slice of run-time length";
    case SysTaskKind::StringCompare: return "a comparison of strings";
    case SysTaskKind::StringMark:
    case SysTaskKind::StringRelease: return "the scratch of strings";
    }
    return "$unknown";
}

std::string_view toString(ParameterMode mode) noexcept {
    switch (mode) {
    case ParameterMode::In: return "in";
    case ParameterMode::Out: return "out";
    case ParameterMode::InOut: return "inout";
    }
    return "?";
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
    case ProcessKind::Override: return "override";
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
    case ProcessKind::Final:
    case ProcessKind::Override: return false;
    }
    return false;
}

} // namespace enzan::eir
