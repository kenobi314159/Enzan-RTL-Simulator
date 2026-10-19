// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Module.h"
#include "eir/Type.h"
#include "frontend/vhdl/Folder.h"
#include "frontend/vhdl/NvcApi.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace enzan::frontend::vhdl {

/// The name EIR gives what VHDL calls `identifier`: its last part after any library and unit
/// prefix, in lower case.
///
/// Lower case because VHDL's basic identifiers are not case-sensitive and NVC holds them in upper
/// case, and every other tool shows them in lower case: GHDL's waveforms, and every testbench's
/// own `report` of a path. An extended identifier, `\Mixed Case\`, is case-sensitive and kept as
/// it is.
[[nodiscard]] std::string simpleName(ident_t identifier);

/// How a value of a VHDL type is laid out in EIR (DESIGN_GUIDE.md section 3.6).
///
/// A scalar is a code: an enumeration its position in as many whole bytes as its values need, so
/// `bit`, `boolean`, `character` and `std_ulogic` are a byte each; an integer signed, in 32 bits
/// when its base type's range fits them and 64 otherwise; a physical value signed and 64 bits, in
/// its primary unit, which for `time` is the femtosecond. The representation is the base type's,
/// so a value of a subtype and one of its base share one: a subtype's narrower range is a check,
/// not a narrower storage.
///
/// An array is its elements end to end, the left element in the highest bits, as a packed array
/// is (section 4.3): `std_logic_vector(7 downto 0)` is 64 bits, element 7 in bits 56 to 63, and
/// `bit_vector(0 to 3)` is 32, element 0 in the top byte. Its bounds are static, since the adapter
/// folds what they depend on (section 5.2). A record is its fields end to end in the order they
/// are declared, the first in the highest bits, as a packed struct is.
struct Layout {
    eir::Type storage;

    /// True for an array, whose index range and elements the fields below describe; a scalar
    /// leaves them at their defaults.
    bool array = false;
    Folder::Range index;
    std::uint32_t elementBits = 0;
    type_t elementType = nullptr;

    /// For an array of one dimension, the layout of each element, made with the bindings the
    /// array's was: so an element's bounds need not be folded again where the array is used, a
    /// generic's actual in its instance's parent say, and an element of an unconstrained array
    /// type, as `slv_array_t`'s `std_logic_vector` is, has the bounds the object gives it.
    std::shared_ptr<const Layout> element;

    /// The dimensions after the first, of an array of more than one: an element of the first is
    /// then a row, an array over these of `elementType`, `elementBits` wide.
    std::vector<Folder::Range> inner;

    /// For a record, its type, and each field's layout and the offset of its bits, in the order
    /// the fields are declared.
    type_t recordType = nullptr;
    std::vector<Layout> fields;
    std::vector<std::uint32_t> fieldOffsets;

    [[nodiscard]] bool isArray() const noexcept { return array; }

    [[nodiscard]] bool isRecord() const noexcept { return recordType != nullptr; }

    /// True for an array or a record, whose value is laid out from parts.
    [[nodiscard]] bool isComposite() const noexcept { return isArray() || isRecord(); }

    /// True for an array of a null range, whose one bit is a placeholder (see `nullLayout`).
    [[nodiscard]] bool isNull() const noexcept { return array && index.length() == 0; }

    /// The offset of the bits of the element at `position` of the index range, which must lie
    /// inside it.
    [[nodiscard]] std::int64_t bitOffset(std::int64_t position) const noexcept {
        return (index.ascending ? index.right - position : position - index.right)
               * static_cast<std::int64_t>(elementBits);
    }
};

/// How `type` is laid out, or nothing when it is a type the adapter cannot hold yet: a real, an
/// access or a file type, or an array whose bounds are not static or hold nothing.
[[nodiscard]] std::optional<Layout> layoutOf(type_t type, const Folder& folder);

/// How the object `declaration` declares is laid out: as its subtype is, or for one of an
/// unconstrained array type, a port taking its bounds from its actual say, with the bounds
/// `folder` binds it to. Nothing when neither gives it bounds, or they hold nothing.
[[nodiscard]] std::optional<Layout> declaredLayout(tree_t declaration, const Folder& folder);

/// The layout of an array of the null range `index`, which holds nothing: a placeholder bit,
/// since a bit vector has at least one, which no element is part of, so that it is a value, a
/// variable or a signal like any other array, and its bounds are what it holds. Its elements'
/// layout is `elementType`'s, or with `elementIndex` as their bounds as for `arrayLayout`; nothing
/// when that is not known, or `index` is not null. Every element is a byte or more, so the
/// placeholder is the width of no other array's value, and a value of it put where one of another
/// length is expected is refused as such.
[[nodiscard]] std::optional<Layout>
nullLayout(const Folder::Range& index, type_t elementType, const Folder& folder,
           const std::optional<Folder::Range>& elementIndex = std::nullopt);

/// `nullLayout` for the object `declaration`, when what it declares is an array of one dimension
/// whose bounds, its subtype's or those `folder` binds it to, are a null range.
[[nodiscard]] std::optional<Layout> declaredNullLayout(tree_t declaration, const Folder& folder);

/// The layout of an array with `index` as its bounds and elements of `elementType`, and with the
/// further dimensions `inner` when it has more than one; or, of one dimension whose element type
/// is an unconstrained array, with `elementIndex` as each element's bounds.
[[nodiscard]] std::optional<Layout>
arrayLayout(const Folder::Range& index, type_t elementType, const Folder& folder,
            const std::vector<Folder::Range>& inner = {},
            const std::optional<Folder::Range>& elementIndex = std::nullopt);

/// The layout of one element of the first dimension of an array laid out as `layout`: its element
/// type's, or a row of the dimensions after it.
[[nodiscard]] std::optional<Layout> elementLayout(const Layout& layout, const Folder& folder);

/// The storage of a scalar `type`, which needs no folding; nothing for anything else.
[[nodiscard]] std::optional<eir::Type> scalarStorage(type_t type);

/// What an object of `type` shows as outside the simulation: `std_ulogic`'s codes, and arrays of
/// them, as four-state bits; `bit`'s and `boolean`'s as two-state ones; a real as the number it
/// is; anything else, an array of reals among it, as its own bits (DESIGN_GUIDE.md section 3.6).
[[nodiscard]] eir::Encoding encodingOf(type_t type);

/// What a signal of `type` resolves with: the resolution function of its subtype, or of its
/// elements' for an array of a resolved subtype, as `std_logic_vector` is, applied to each
/// subelement it resolves (IEEE 1076 section 4.6), scalar or composite, a composite one laid out
/// with what `folder` binds. Nothing for an unresolved type, and for one whose resolution is not
/// held yet: a record's resolved element by element, or a composite whose layout does not fold.
[[nodiscard]] std::optional<eir::Resolution> resolutionOf(type_t type, const Folder& folder);

/// True when `type` is resolved by a resolution `resolutionOf` does not hold, so that a signal of
/// it is refused rather than left unresolved.
[[nodiscard]] bool resolutionNotHeld(type_t type, const Folder& folder);

/// The bounds of the array the resolution function `function` is given the values of `drivers`
/// drivers in: ascending from the left bound of its parameter's index subtype, as NVC gives them,
/// so `0 to n - 1` for `std_ulogic_vector`'s natural index and `1 to n` for a positive one.
/// Nothing when that bound does not fold.
[[nodiscard]] std::optional<Folder::Range> driversRange(tree_t function, std::uint32_t drivers);

/// True when `type` is an integer or physical type, whose values compare and compute signed.
[[nodiscard]] bool isSignedScalar(type_t type);

/// The enumeration `type` is, for display, or nothing when it is not one.
[[nodiscard]] std::optional<eir::EnumDefinition> enumerationOf(type_t type);

/// The bits of `value` held as `type` holds it.
[[nodiscard]] eir::BitVector encode(std::int64_t value, eir::Type type);

/// The bits of `expression`, a static value laid out as `layout`, or nothing when it is not one
/// the adapter can fold: a literal, a string, an aggregate of static elements, a constant or a
/// generic, or an expression of them that `Folder` folds.
///
/// The layout is the context's, not the expression's own, because an aggregate with `others`
/// takes its bounds from where it is used.
[[nodiscard]] std::optional<eir::BitVector> constantBits(tree_t expression, const Layout& layout,
                                                         const Folder& folder);

/// The bits of `expression`, a field of a static record, `r.x` of a generic or of a constant;
/// nothing when the record does not fold.
[[nodiscard]] std::optional<eir::BitVector> fieldBits(tree_t expression, const Folder& folder);

/// The bounds of `expression`, a static array of `type`: its own, or for one that has none, a
/// concatenation say, as many elements as it folds to from the left of the index subtype.
[[nodiscard]] std::optional<Folder::Range> constantBounds(tree_t expression, type_t type,
                                                          const Folder& folder);

/// The value a `layout` object starts with when its declaration gives none: each scalar its
/// type's left bound (IEEE 1076 section 14.4.2.5).
[[nodiscard]] std::optional<eir::BitVector> defaultBits(type_t type, const Layout& layout,
                                                        const Folder& folder);

} // namespace enzan::frontend::vhdl
