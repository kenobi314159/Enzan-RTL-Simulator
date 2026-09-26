// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/Protocol.h"

#include <format>

namespace enzan::ipc {

std::string toString(MessageType type) {
    switch (type) {
    case MessageType::Hello: return "hello";
    case MessageType::QueryDesign: return "query-design";
    case MessageType::QueryObjects: return "query-objects";
    case MessageType::Subscribe: return "subscribe";
    case MessageType::Unsubscribe: return "unsubscribe";
    case MessageType::QueryHistory: return "query-history";
    case MessageType::Run: return "run";
    case MessageType::Stop: return "stop";
    case MessageType::Restart: return "restart";
    case MessageType::Quit: return "quit";
    case MessageType::Detach: return "detach";
    case MessageType::QuerySource: return "query-source";
    case MessageType::SetBreakpoint: return "set-breakpoint";
    case MessageType::ClearBreakpoint: return "clear-breakpoint";
    case MessageType::EnableBreakpoint: return "enable-breakpoint";
    case MessageType::Step: return "step";
    case MessageType::Deposit: return "deposit";
    case MessageType::Force: return "force";
    case MessageType::Welcome: return "welcome";
    case MessageType::Design: return "design";
    case MessageType::Objects: return "objects";
    case MessageType::ValueChanges: return "value-changes";
    case MessageType::History: return "history";
    case MessageType::Transcript: return "transcript";
    case MessageType::Diagnostic: return "diagnostic";
    case MessageType::RunState: return "run-state";
    case MessageType::Bye: return "bye";
    case MessageType::Failure: return "failure";
    case MessageType::Source: return "source";
    case MessageType::Breakpoints: return "breakpoints";
    }

    // A number this build does not know is reported as the number. Guessing a name for it
    // would make a version skew read as a message that exists.
    return std::format("message({})", static_cast<std::uint16_t>(type));
}

std::string_view toString(SimulationState state) {
    switch (state) {
    case SimulationState::Idle: return "idle";
    case SimulationState::Running: return "running";
    case SimulationState::Finished: return "finished";
    case SimulationState::Failed: return "failed";
    }
    return "failed";
}

} // namespace enzan::ipc
