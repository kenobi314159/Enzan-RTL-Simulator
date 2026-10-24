// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What PliContext holds of the design and of an application's handles and callbacks, shared by
// the files implementing it: PliContext.cpp, which has VPI's routines, and Vhpi.cpp, which has
// VHPI's. Not part of the context's interface, and included by nothing else.

#include "pli/PliContext.h"

#include "common/SourceLocation.h"

#include <cstdarg>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace enzan::pli {

/// No scope: the parent of a top-level instance, or the scope of an object an application
/// cannot see.
inline constexpr std::uint32_t NoScope = 0xFFFF'FFFFU;

/// Anything an application holds a handle to. The handle is this object's address.
struct PliContext::Object {
    enum class Kind : std::uint8_t {
        Scope,
        Signal,
        Parameter,
        Constant,
        Iterator,
        Callback,
        /// A VHPI handle, which `vhpiClass` says the class of (DESIGN_GUIDE.md section 9.7). The
        /// two interfaces' handles never stand for one another: each refuses the other's.
        Vhpi
    };

    Object() = default;
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&&) = delete;
    Object& operator=(Object&&) = delete;
    virtual ~Object() = default;

    Kind kind = Kind::Scope;
    PLI_INT32 type = vpiUndefined;
    Target target;

    /// The scope a signal or parameter is in, or the scope itself.
    std::uint32_t scope = NoScope;

    /// The slot a signal is, which is also its signal index in the image.
    std::uint32_t signal = 0;

    std::uint32_t width = 0;
    bool isSigned = false;

    /// For a bit-select or an element of an array, the window of the signal it is: `width`
    /// bits from `lowBit`.
    bool isWindow = false;
    bool isElement = false;
    std::uint32_t lowBit = 0;

    /// For an array, the unpacked dimensions not yet indexed, outermost first; its own bounds
    /// are the first one's. An array has elements and no value of its own: arrays themselves
    /// while dimensions remain below them, vectors `elementWidth` wide at the last.
    bool isArray = false;
    std::vector<eir::UnpackedDimension> dimensions;
    std::int64_t left = 0;
    std::int64_t right = 0;
    std::uint32_t elementWidth = 0;

    /// An element's index in the dimension it was selected from, as declared.
    std::int64_t index = 0;

    std::string name;
    std::string fullName;
    std::string definitionName;
    std::string file;
    std::uint32_t line = 0;

    /// What an iterator returns, and how far it has got.
    std::vector<vpiHandle> items;
    std::size_t position = 0;
    PLI_INT32 iteratorType = 0;

    // -- VHPI ---------------------------------------------------------------

    /// The class of a VHPI handle, `vhpiKindP`: a `vhpiClassKindT`.
    std::int32_t vhpiClass = 0;

    /// The instance whose definition's table of VHDL types `vhdlType` indexes, and the type: of
    /// the object a handle stands for, or the type a type handle is. A type handle stands for the
    /// base type when `baseType` is set, whose array bounds are not its subtype's.
    std::uint32_t instance = 0;
    std::uint32_t vhdlType = eir::VhdlType::None;
    bool baseType = false;

    /// What a parameter, a literal, a bound or an argument handle says of itself.
    std::int64_t number = 0;
    std::string text;

    /// `name` and `fullName` as VHPI gives them, in upper case: made the first time asked for and
    /// kept with the handle, so that a string `vhpi_get_str` returned stays valid however many it
    /// returns after it, as VPI's do. An application holding two at once, as cocotb's messages
    /// do, would otherwise read freed memory once the second outgrew the first.
    mutable std::string vhpiName;
    mutable std::string vhpiFullName;

    /// Where a declared VHDL object is declared, at its name, whose spelling its CaseName
    /// properties give, and those names made from it the first time they are asked for.
    SourceLocation declaredAt;
    mutable std::string vhpiCaseName;
    mutable std::string vhpiFullCaseName;

    /// What a VHPI iterator has yet to return, made as it is scanned; owned until then.
    std::vector<std::unique_ptr<Object>> pending;
};

/// A registered callback. Its handle is what `vpi_register_cb` returned.
struct PliContext::Callback final : Object {
    s_cb_data data{};

    /// How the application asked for time and value to be reported to it.
    PLI_INT32 timeType = vpiSuppressTime;
    PLI_INT32 valueFormat = vpiSuppressVal;

    /// Set when the callback has fired for the last time or been removed. It is not freed
    /// until no dispatch can still be holding it.
    bool removed = false;

    /// What the window a value-change callback on a bit-select or an element watches held when
    /// it last fired. The kernel reports changes of whole signals, and a change elsewhere in
    /// the signal is not a change of this window.
    eir::BitVector lastValue{1};

#ifdef ENZAN_WITH_VHDL
    /// For a VHPI callback, what it was registered with, the handle it watches, and the format
    /// it asked for the value in, if it asked for one; `vhpiData.obj` is that handle.
    vhpiCbDataT vhpiData{};
    std::optional<vhpiFormatT> vhpiValueFormat;

    /// VHPI's repetitive callbacks, `vhpiCbRep...`, which fire at every occurrence until they are
    /// removed rather than once.
    bool repeats = false;
#endif
};

/// A module instance or a generate block.
struct PliContext::Scope {
    bool isModule = true;

    /// The instance holding a package's variables, which is at the root but is no top-level
    /// module: iterating the root's modules skips it, so a tool looking for the design's top
    /// does not find a package instead.
    bool isPackage = false;

    /// Whether it is of a VHDL definition. Each interface shows its own language (DESIGN_GUIDE.md
    /// section 18, M7): VPI finds nothing of VHDL by name, and an iteration of a SystemVerilog
    /// scope returns a VHDL instance in it as an object of a type VPI does not describe; VHPI shows
    /// a SystemVerilog instance as a region of class `vhpiVerilog` and nothing in it.
    bool vhdl = false;
    std::uint32_t instance = 0;
    std::uint32_t parent = NoScope;

    /// What the names of this scope's objects begin with inside their instance: empty for the
    /// instance itself, `gen[0].` for a generate block, since the frontend flattens generate
    /// blocks into the names it gives their objects.
    std::string prefix;

    std::string name;
    std::string fullName;

    /// Generate blocks first, then instances, each in the order they were met.
    std::vector<std::uint32_t> children;

    /// Indices into the instance's definition.
    std::vector<std::uint32_t> signals;
    std::vector<std::uint32_t> parameters;

    /// The objects VHPI shows here: every one with a VHDL type, a real's among them, which VPI
    /// does not show (section 9.7).
    std::vector<std::uint32_t> vhdlObjects;
};

/// Formats `text` with `arguments` as `vprintf` would, into a string: what `vpi_printf`,
/// `vhpi_printf` and their relatives print.
std::string formatted(const char* text, va_list arguments);

} // namespace enzan::pli
