// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

/// Whether an object is a net or a variable.
///
/// The distinction is not cosmetic. A bit of a net may have several drivers and
/// resolves them; a bit of a variable may have one continuous driver, and a second is
/// an error rather than a resolution (DESIGN_GUIDE.md section 3.7). Elaboration
/// decides which applies from this.
enum class ObjectKind : std::uint8_t { Net, Variable };

std::string_view toString(ObjectKind kind) noexcept;

/// What an object's bits mean outside the simulation: in a waveform, to a display, across a
/// language boundary (DESIGN_GUIDE.md section 3.6).
///
/// The kernel never looks: a VHDL value is a code, and every operation on it is an operation on
/// bits. Only what shows a value to someone converts it, each element of one byte to the bit it
/// stands for.
enum class Encoding : std::uint8_t {
    /// The bits are the value, as every SystemVerilog object's are.
    Bits,

    /// `std_ulogic` codes, a byte each, in the order of its literals: U X 0 1 Z W L H -. Each is
    /// one four-state bit, L and H their strengths' values, and U, W and - unknown.
    VhdlLogic,

    /// `bit` or `boolean` codes, a byte each: 0 or 1.
    VhdlBit,

    /// An IEEE 754 double's 64 bits, a real's (section 3.6), which a waveform shows as the
    /// number rather than as bits.
    Real,
};

/// How many bits an object of `width` bits stored as `encoding` shows as.
[[nodiscard]] std::uint32_t presentedWidth(Encoding encoding, std::uint32_t width) noexcept;

/// Writes into `shown`, `presentedWidth` bits wide, the value `stored` shows as.
void present(Encoding encoding, ConstBitRef stored, BitRef shown) noexcept;

/// Writes into `stored` the value that shows as `shown`, `presentedWidth` of its width: what a
/// reader writing a signal means by the bits they give, `1` a std_ulogic's '1'. An unknown bit is
/// 'X' and a high-impedance one 'Z'. False, `stored` left as it was, when a bit has no code: an
/// unknown or high-impedance `bit` or `boolean`.
[[nodiscard]] bool storeShown(Encoding encoding, ConstBitRef shown, BitRef stored) noexcept;

/// As `storeShown`, for a value that has to arrive whatever it holds, as one crossing from
/// SystemVerilog into VHDL does (section 3.5): an unknown or high-impedance bit becomes a `bit`'s
/// or a `boolean`'s 0, as assigning one to a two-state variable makes it.
void encode(Encoding encoding, ConstBitRef shown, BitRef stored) noexcept;

/// What an object stored as `encoding` holds, for a diagnostic: `std_ulogic codes`.
[[nodiscard]] std::string_view toString(Encoding encoding) noexcept;

/// The language a definition was written in.
///
/// Nothing about simulating a definition depends on it: what its bits mean is its objects'
/// encodings. It says where a port crosses from one language into the other, which is connected by
/// a conversion that consumes a delta cycle (DESIGN_GUIDE.md section 3.5), and which procedural
/// interface shows the definition (section 9.7).
enum class Language : std::uint8_t { SystemVerilog, Vhdl };

/// Which kind of net a net is, which decides how a bit with several drivers resolves and what
/// a bit with none holds (DESIGN_GUIDE.md section 3.7).
///
/// `tri` is kept apart from `wire`, and `triand` and `trior` from `wand` and `wor`, although
/// each pair behaves identically: a reader is shown the kind they declared.
enum class NetKind : std::uint8_t {
    Wire,
    Tri,
    WiredAnd,
    WiredOr,
    TriAnd,
    TriOr,
    /// Pulled to 0 where nothing drives it, and where every driver is `z`.
    Tri0,
    /// Pulled to 1 likewise.
    Tri1,
    /// Always 0. Its supply strength beats every driver Enzan can model, all of which are
    /// strong.
    Supply0,
    /// Always 1.
    Supply1,
    /// A net that may have one driver per bit, as a variable may.
    UWire,
};

/// The keyword that declares `kind`, for diagnostics and the GUI.
std::string_view toString(NetKind kind) noexcept;

/// The value of a bit of a `kind` net that nothing drives: `z`, or the value it is pulled or
/// tied to.
Logic undrivenValue(NetKind kind) noexcept;

/// True when even one driver's value has to be resolved before it reaches the net: a pulled
/// net turns a driven `z` into its pull, and a supply net ignores its drivers.
bool resolvesSingleDrivers(NetKind kind) noexcept;

/// One name an enumeration gives to one value.
struct EnumMember {
    std::string name;
    BitVector value;
};

/// An enumeration type: somewhere to look up what a value is called.
///
/// EIR does not model enumerations as *types* — an enumeration is a packed vector as far as
/// every operation on it is concerned, and pretending otherwise would put a distinction into
/// the value engine that nothing there acts on. What is kept is the mapping, because a reader
/// looking at `3'd2` on a waveform wants to be told it is `S_SEND` (DESIGN_GUIDE.md section
/// 10.7) and only the frontend ever knew that.
///
/// Held per definition rather than per design, and copied into each definition that uses one,
/// so that a definition stays an independently usable analysed unit (section 4.4).
struct EnumDefinition {
    /// What the type is called, for a tooltip and for diagnostics. May be empty: an anonymous
    /// enumeration still names its values.
    std::string name;

    /// In declaration order, which is the order a reader wrote them and the order anything
    /// listing them must use (section 3.8).
    std::vector<EnumMember> members;

    /// The name for `value`, or an empty view when no member has it.
    ///
    /// A linear scan over a handful of members. Two members may share a value, which IEEE 1800
    /// allows; the first wins, so the answer is the one written first rather than whichever a
    /// container happened to keep.
    [[nodiscard]] std::string_view nameOf(const BitVector& value) const;
};

/// A VHDL type as the design declares it, for what reads a VHDL object from outside the
/// simulation: VHPI's types, bounds and subelements (DESIGN_GUIDE.md section 9.7). Like an
/// enumeration's table, it changes nothing about how the object behaves; its bits are laid out as
/// the VHDL adapter lays them out, an array's left element and a record's first field highest.
struct VhdlType {
    enum class Kind : std::uint8_t { Enumeration, Integer, Physical, Floating, Array, Record };

    /// A range of a scalar type, or of one dimension of an array, as declared: `7 downto 0`
    /// is a left of 7, a right of 0 and not ascending. An enumeration's is of positions.
    struct Range {
        std::int64_t left = 0;
        std::int64_t right = 0;
        bool ascending = true;

        friend bool operator==(const Range&, const Range&) noexcept = default;
    };

    /// A field of a record: its name, its type in the same table, and the offset of its bits.
    struct Field {
        std::string name;
        std::uint32_t type = 0;
        std::uint32_t offset = 0;

        friend bool operator==(const Field&, const Field&) noexcept = default;
    };

    static constexpr std::uint32_t None = 0xFFFFFFFFU;

    Kind kind = Kind::Integer;

    /// The name of the type, or of its base type when it is an anonymous subtype, as VHDL
    /// writes it in upper case: `STD_ULOGIC_VECTOR`, `INTEGER`.
    std::string name;

    std::uint32_t width = 0;

    /// What a scalar's bits mean, as an object's encoding says (section 3.6): std_ulogic codes, a
    /// bit's or a boolean's, or the value itself. `Bits` for an array or a record, whose elements
    /// and fields say for themselves.
    Encoding encoding = Encoding::Bits;

    /// A scalar's range, or each dimension of an array, the first first.
    std::vector<Range> ranges;

    /// An enumeration's literals, an index into its definition's table of enumerations.
    std::uint32_t enumIndex = None;

    /// An array's element type, an index into the same table.
    std::uint32_t element = None;

    /// A record's fields, in the order they are declared.
    std::vector<Field> fields;

    friend bool operator==(const VhdlType&, const VhdlType&) noexcept = default;
};

/// One unpacked dimension of an array, as declared: `[0:15]` is a left of 0 and a right of 15.
///
/// Kept for what reads the array as an array — VPI's `vpiRegArray`, the waveform database's
/// choice not to record memories — and not for simulating it. Storage is flat: the array's
/// elements are one vector of its bits, left element highest, as a packed array's are
/// (DESIGN_GUIDE.md section 4.3), so every operation on it is an operation on bits.
struct UnpackedDimension {
    std::int32_t left = 0;
    std::int32_t right = 0;

    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(left > right ? left - right : right - left) + 1U;
    }

    friend bool operator==(UnpackedDimension, UnpackedDimension) noexcept = default;
};

/// What a VHDL resolved signal's drivers resolve with (IEEE 1076 section 4.6): a resolution
/// function, applied to each subelement of the signal it resolves, `elementBits` wide, over the
/// values its drivers give it (DESIGN_GUIDE.md section 3.7). The subelement is a scalar, or for a
/// function of composite values an array or a record, which each driver drives whole.
///
/// The function is named as the design names it, by its library, package and signature as NVC
/// mangles them, `IEEE.STD_LOGIC_1164.RESOLVED(Y)U`, rather than as a subprogram of EIR: its one
/// parameter is an array as long as the subelement has drivers, which only elaboration knows, so
/// elaboration asks the frontend for it by this name once it does.
struct Resolution {
    std::string function;
    std::uint32_t elementBits = 0;

    /// True when the function is proven to return a lone driver's value unchanged, having been
    /// run on each value of its subelement, an enumeration of sixteen values or fewer, as
    /// `ieee.std_logic_1164.resolved` is. Bits with one driver then bypass it (section 3.7).
    bool loneDriverIdentity = false;

    [[nodiscard]] bool resolved() const noexcept { return !function.empty(); }

    friend bool operator==(const Resolution&, const Resolution&) = default;
};

/// A declared signal or variable inside a module.
struct Object {
    std::string name;
    Type type;
    ObjectKind kind = ObjectKind::Variable;

    /// The kind of net, when `kind` is `Net`; ignored for a variable.
    NetKind netKind = NetKind::Wire;

    SourceLocation location;

    /// The value at time zero, when the declaration supplied one.
    ///
    /// Absent means "the default for the type": X for a four-state object and zero
    /// for a two-state one (section 3.9). It is left absent rather than filled in
    /// here so that elaboration can initialise a whole arena region at once instead
    /// of copying a vector per object.
    std::optional<BitVector> initialValue;

    /// True when the object is only a name for another object's storage, which is
    /// what a collapsed port connection produces. Elaboration sets this; frontends
    /// never do.
    bool isAlias = false;

    /// What the bits mean outside the simulation: VHDL's codes, or the value itself.
    Encoding encoding = Encoding::Bits;

    /// For a VHDL resolved signal, the function its drivers resolve with; empty otherwise.
    Resolution resolution;

    /// The enumeration this object's type is, as an index into its definition's table, or
    /// `NoEnum`. It changes nothing about how the object behaves; it is what lets a display
    /// name the value.
    std::uint32_t enumIndex = NoEnum;

    /// The unpacked dimensions, outermost first, when the object is an unpacked array; empty
    /// otherwise. Its type is then the flat vector of all its elements.
    std::vector<UnpackedDimension> unpackedDimensions;

    /// For a VHDL object, its type, an index into its definition's `vhdlTypes`, or
    /// VhdlType::None.
    std::uint32_t vhdlType = VhdlType::None;

    /// True for a VHDL signal of a null range, which holds nothing: its one bit is a placeholder,
    /// a bit vector having at least one, that nothing reads, writes or records. VHPI shows it as
    /// the empty array its type says it is.
    bool holdsNothing = false;

    /// The width of one element, when `unpackedDimensions` is not empty.
    std::uint32_t elementWidth = 0;

    /// Whether one element is signed, when `unpackedDimensions` is not empty. The flat vector
    /// is unsigned whatever its elements are, so this is the only record of it.
    bool elementSigned = false;

    /// A static variable of a function or task: one variable that every call of it shares, so
    /// that what one call leaves there the next reads (IEEE 1800 section 13.3.1), written by
    /// whichever process makes the call. A call inlined into a continuous assignment is no
    /// continuous driver of it, and an inferred sensitivity list does not watch it: what a
    /// call writes there is the call's own business, not a change the caller depends on.
    bool subroutineStatic = false;

    /// When not empty, the object is a hierarchical reference: a name here for an object of
    /// another instance, which elaboration binds to that object's storage.
    ///
    /// The path is relative to the instance holding this object — `dut/core/state` down,
    /// `../u_peer/ready` up and across — because one definition serves every instance of a
    /// parameterisation (DESIGN_GUIDE.md section 18, M3.1), and a path from the root would be
    /// right for only one of them. A path beginning `@port/` goes through an interface port of
    /// the instance holding it, to whichever interface that instance's parent connected.
    std::string hierarchicalPath;

    static constexpr std::uint32_t NoEnum = 0xFFFF'FFFFU;
};

/// A parameter or localparam of a module definition, kept for display.
///
/// Parameters are resolved away before EIR exists: a parameterised module becomes one
/// definition per parameterisation, which is what keeps an analysed unit free of them and
/// reusable without re-evaluating anything (DESIGN_GUIDE.md section 4.4). What is kept here is
/// the *record* of what each was resolved to, because a reader debugging a design built out of
/// three differently-parameterised copies of one module needs to see which copy they are
/// looking at, and nothing downstream of the frontend could work that out.
struct Parameter {
    std::string name;

    BitVector value;

    /// True for `localparam`, and for a VHDL constant the definition declares. Worth
    /// distinguishing on screen: one is set from outside the module and the other is derived
    /// inside it, which is the difference between "why is this instance configured like that"
    /// and "what did that work out to".
    bool isLocal = false;

    SourceLocation location;

    /// For a VHDL generic or constant, its type, an index into its definition's `vhdlTypes`, or
    /// VhdlType::None.
    std::uint32_t vhdlType = VhdlType::None;
};

/// Direction of a module port.
enum class PortDirection : std::uint8_t { Input, Output, InOut };

std::string_view toString(PortDirection direction) noexcept;

/// A port of a module definition.
struct Port {
    std::string name;
    PortDirection direction = PortDirection::Input;

    /// The object inside this module that the port names.
    std::uint32_t objectIndex = 0;

    SourceLocation location;
};

/// One binding of an instance's port to an object in the instantiating module.
///
/// A connection names an object, never an expression. The frontend normalises
/// anything more complicated — a concatenation, a literal, a sliced signal — into an
/// extra object in the parent plus a continuous assignment that computes it. That
/// keeps elaboration's job to connecting storage rather than compiling expressions,
/// and it means the extra delta such a connection costs is visible in the EIR rather
/// than hidden in the elaborator.
struct PortConnection {
    /// Index into the instantiated definition's port list.
    std::uint32_t portIndex = 0;

    /// Index into the instantiating module's object table, or `Unconnected`.
    std::uint32_t objectIndex = Unconnected;

    /// The object the instance's drivers of the port drive, when it is not the one the port
    /// reads; otherwise `Unconnected`, and they drive `objectIndex`.
    ///
    /// A VHDL inout port associated with part of a signal or through a conversion reads its
    /// effective value and drives its driving value, which differ (IEEE 1076 section 14.7.3):
    /// what the instance drives is one source of the actual, and what it reads is the actual
    /// resolved, converted back. The frontend connects the two objects to the actual by
    /// processes of its own (DESIGN_GUIDE.md section 5.2); elaboration only routes the
    /// instance's stores.
    std::uint32_t driveObjectIndex = Unconnected;

    static constexpr std::uint32_t Unconnected = 0xFFFF'FFFFU;

    [[nodiscard]] bool connected() const noexcept { return objectIndex != Unconnected; }

    /// The object the instance's drivers of the port drive.
    [[nodiscard]] std::uint32_t drivenObject() const noexcept {
        return driveObjectIndex == Unconnected ? objectIndex : driveObjectIndex;
    }
};

/// What an instance's interface port is connected to (IEEE 1800 section 25).
///
/// The target is named from the instantiating module, as a hierarchical reference's path is: an
/// interface instance in it, by the name it has there, or `@port` for an interface port of its
/// own, which is how an interface is passed down through several levels.
struct InterfaceBinding {
    std::string port;
    std::string target;

    friend bool operator==(const InterfaceBinding&, const InterfaceBinding&) = default;
};

/// An instantiation of one module definition inside another.
struct Instance {
    /// The instance name, as it appears in the hierarchical path.
    std::string name;

    /// The definition this instantiates, resolved against the Design by name.
    ///
    /// A name rather than an index because analysis is per-design-unit and
    /// cacheable (section 2.1): a module may be analysed before the thing it
    /// instantiates exists in any library, so the reference cannot be an index into
    /// a table that is not yet complete. Elaboration resolves it and reports an
    /// unresolved reference as a diagnostic.
    std::string definitionName;

    std::vector<PortConnection> connections;

    /// The instance's interface ports, by name, and what each is connected to.
    std::vector<InterfaceBinding> interfaceBindings;

    SourceLocation location;
};

/// One analysed design unit: a module, with its objects, ports, instances and
/// processes.
///
/// This is EIR-S, the structural layer (section 4.1), and it is the unit that
/// analysis produces and a library stores. It describes the module as written, with
/// parameters already resolved but with **no** hierarchy flattening: flattening is
/// an elaboration-time optimisation and never a frontend's job (section 17.4).
struct ModuleDefinition {
    /// The name this definition is instantiated by.
    ///
    /// For a parameterised module the frontend mangles the parameter values into the
    /// name, so that each distinct parameterisation is its own definition. That keeps
    /// EIR-S free of parameters entirely, which is what makes an analysed unit
    /// reusable without re-evaluating anything.
    std::string name;

    /// The name the user wrote, before parameter mangling, for diagnostics and for
    /// the hierarchy display.
    std::string sourceName;

    /// The logical library this unit was analysed into, or empty when it was not
    /// analysed into one.
    ///
    /// Carried on the unit rather than held beside the design, because a design
    /// assembled from several libraries must be able to say which of them each unit
    /// came from: two libraries may define one name, and resolving that by whichever
    /// was loaded last — which is what a design keyed on the bare name does — is the
    /// failure mode section 1.2 ranks worst. Section 8.2 has the resolution rules.
    std::string library;

    /// True for a package's variables rather than a module (IEEE 1800 section 26): one
    /// instance of it exists, at the root as `/<package>`, when some module refers to one of
    /// them, and it is never a top level. Named `<package>::`, which no module can be.
    bool isPackage = false;

    /// The language the definition was written in.
    Language language = Language::SystemVerilog;

    SourceLocation location;

    std::vector<Object> objects;
    std::vector<Port> ports;
    std::vector<Instance> instances;
    std::vector<Process> processes;

    /// The subprograms this definition declares, which calls name by definition and name
    /// (`CallSite`). A package's are its own, so one definition serves every caller.
    std::vector<Subprogram> subprograms;

    /// The enumerations objects here refer to by index.
    std::vector<EnumDefinition> enums;

    /// The VHDL types this definition's objects are of, which they and each other index.
    std::vector<VhdlType> vhdlTypes;

    /// What this parameterisation's parameters were resolved to, in declaration order.
    std::vector<Parameter> parameters;

    /// Returns the index of the object called `name`, or nothing.
    ///
    /// A linear scan: an object table is small, and a map would add a container
    /// whose iteration order could leak into output ordering (section 3.8).
    [[nodiscard]] std::optional<std::uint32_t> findObject(std::string_view wanted) const;

    /// Adds an object and returns its index.
    std::uint32_t addObject(Object object);

    /// Returns the index of the subprogram called `name`, or nothing.
    ///
    /// A linear scan, as `findObject` is, and for the same reasons.
    [[nodiscard]] std::optional<std::uint32_t> findSubprogram(std::string_view wanted) const;

    /// Adds an enumeration, or returns the index of an identical one already here.
    ///
    /// Deduplicated because one enumeration usually declares several signals, and a table with
    /// a copy per signal would send the same members over the wire once per object.
    std::uint32_t addEnum(EnumDefinition definition);
};

} // namespace enzan::eir
