// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Instruction.h"

namespace enzan::eir {

namespace {

struct OpcodeInfo {
    std::string_view mnemonic;
    bool hasResult;
    std::uint8_t sourceCount;
    std::string_view llvmLowering;
};

// One table, generated from the catalogue, so that a new opcode cannot be added
// without every property being supplied.
constexpr OpcodeInfo OpcodeTable[] = {
#define ENZAN_OPCODE(name, mnemonicText, resultFlag, sources, lowering) \
    OpcodeInfo{mnemonicText, resultFlag, sources, lowering},
#include "eir/Opcodes.def"
#undef ENZAN_OPCODE
};

const OpcodeInfo& info(Opcode opcode) noexcept {
    const auto index = static_cast<std::size_t>(opcode);
    return OpcodeTable[index < std::size(OpcodeTable) ? index : 0];
}

} // namespace

std::string_view mnemonic(Opcode opcode) noexcept {
    return info(opcode).mnemonic;
}

bool hasResult(Opcode opcode) noexcept {
    return info(opcode).hasResult;
}

std::uint8_t sourceCount(Opcode opcode) noexcept {
    return info(opcode).sourceCount;
}

std::string_view llvmLowering(Opcode opcode) noexcept {
    return info(opcode).llvmLowering;
}

bool suspends(Opcode opcode) noexcept {
    return opcode == Opcode::WaitTime || opcode == Opcode::WaitEdge || opcode == Opcode::WaitEvents
           || opcode == Opcode::WaitEventsFor;
}

bool isTerminator(Opcode opcode) noexcept {
    // A suspension ends the block as surely as a branch does: control leaves for
    // the scheduler and comes back at the named resume block.
    return opcode == Opcode::Jump || opcode == Opcode::BranchIf || opcode == Opcode::Return
           || suspends(opcode);
}

bool isStore(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Store:
    case Opcode::StorePart:
    case Opcode::StoreNonBlocking:
    case Opcode::StorePartNonBlocking:
    case Opcode::StoreDelayed:
    case Opcode::StoreTransport:
    case Opcode::StoreNonBlockingDelayed:
    case Opcode::Drive:
    case Opcode::DriveTransport:
    case Opcode::DrivePart:
    case Opcode::DrivePartAfter:
    case Opcode::DriveTransportPart: return true;
    default: return false;
    }
}

bool allowsAliasedDestination(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Move:
    case Opcode::Extend:
    case Opcode::Truncate:
    case Opcode::CoerceTwoState:
    case Opcode::SplicePart: return true;
    default: return false;
    }
}

std::string_view toString(EdgeKind edge) noexcept {
    switch (edge) {
    case EdgeKind::AnyChange: return "change";
    case EdgeKind::Posedge: return "posedge";
    case EdgeKind::Negedge: return "negedge";
    }
    return "change";
}

} // namespace enzan::eir
