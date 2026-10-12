// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Module.h"
#include "eir/Type.h"
#include "frontend/vhdl/NvcApi.h"

#include <cstdint>
#include <optional>
#include <string>

namespace enzan::frontend::vhdl {

/// The name EIR gives what VHDL calls `identifier`: its last part after any library and unit
/// prefix, in lower case.
///
/// Lower case because VHDL's basic identifiers are not case-sensitive and NVC holds them in upper
/// case, and every other tool shows them in lower case: GHDL's waveforms, and every testbench's
/// own `report` of a path. An extended identifier, `\Mixed Case\`, is case-sensitive and kept as
/// it is.
[[nodiscard]] std::string simpleName(ident_t identifier);

/// How a value of `type` is held in EIR, or nothing when it is a type the adapter cannot hold yet.
///
/// DESIGN_GUIDE.md section 3.6: an enumeration is its position in as many whole bytes as its
/// values need, so `bit`, `boolean`, `character` and `std_ulogic` are a byte each; an integer is
/// signed, in 32 bits when its base type's range fits them and 64 otherwise; a physical value is
/// signed and 64 bits, in its primary unit, which for `time` is the femtosecond.
///
/// The representation is the base type's, so a value of a subtype and one of its base share one
/// and convert without an instruction. A subtype's narrower range is a check (`natural` refusing
/// -1), not a narrower storage.
[[nodiscard]] std::optional<eir::Type> storageOf(type_t type);

/// True when `type` is an integer or physical type, whose values compare and compute signed.
[[nodiscard]] bool isSignedScalar(type_t type);

/// The enumeration `type` is, for display, or nothing when it is not one.
[[nodiscard]] std::optional<eir::EnumDefinition> enumerationOf(type_t type);

/// The bits of `value` held as `type` holds it.
[[nodiscard]] eir::BitVector encode(std::int64_t value, eir::Type type);

} // namespace enzan::frontend::vhdl
