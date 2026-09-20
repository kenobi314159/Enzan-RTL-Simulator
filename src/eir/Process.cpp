// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Process.h"

namespace enzan::eir {

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
