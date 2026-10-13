// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pli/VpiValue.h"
#include "runtime/Kernel.h"
#include "runtime/Time.h"

#include "vpi_user.h"

#ifdef ENZAN_WITH_VHDL
#include "vhpi_user.h"
#endif

#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan {
class DiagnosticEngine;
class SourceManager;
} // namespace enzan

namespace enzan::runtime {
class Image;
}

namespace enzan::pli {

/// The procedural interfaces of one `enzan` process: the Verilog Procedural Interface
/// (DESIGN_GUIDE.md section 9), and the VHDL one beside it (section 9.7), which share one model
/// of the design and one set of callbacks, so that an application using both, as cocotb does
/// with a design of both languages, sees one simulation.
///
/// Holds everything an application can reach: the libraries it was loaded from, the handles
/// it has been given, the callbacks it has registered and the last error. The `vpi_*` routines
/// carry no context argument, so they find this one through `current`; there is one per
/// process, which is also what a session is (src/session/README.md).
///
/// An application sees the elaborated design (section 9.2) and drives the kernel through the
/// same operations the front end uses (sections 9.4 and 10.4). It reaches the region ordering
/// through the kernel's `PliHooks`, which this implements.
///
/// Loading comes first and needs no design; `attach` binds one when elaboration has produced
/// it, and from then until `detach` the application can see and drive it.
class PliContext final : public runtime::PliHooks {
public:
    /// `diagnostics` must outlive the context. `commandLine` is what `vpi_get_vlog_info`
    /// reports: the whole command line, so that an application finds its plusargs there.
    PliContext(DiagnosticEngine& engine, std::vector<std::string> commandLine);

    PliContext(const PliContext&) = delete;
    PliContext& operator=(const PliContext&) = delete;
    PliContext(PliContext&&) = delete;
    PliContext& operator=(PliContext&&) = delete;
    ~PliContext() override;

    /// The context the `vpi_*` routines act on, or null when none exists.
    static PliContext* current() noexcept;

    /// The interface an application is loaded through, which says what its table of startup
    /// routines is called.
    enum class Interface : std::uint8_t { Vpi, Vhpi };

    /// Loads `library[:entry]` and runs its startup routines (sections 9.1 and 9.7).
    ///
    /// Returns false having reported why when the library cannot be opened, or offers neither
    /// the entry it was asked for nor the interface's table, `vlog_startup_routines` or
    /// `vhpi_startup_routines`.
    bool load(std::string_view specification, Interface which = Interface::Vpi);

    /// True once at least one application has been loaded.
    [[nodiscard]] bool hasApplications() const noexcept { return loadedLibraries > 0; }

    /// Binds an elaborated design and the kernel running it.
    ///
    /// `image` and `kernel` must stay valid until `detach`. `sources`, when given, is how a
    /// handle's file is named; a design built by hand has none.
    void attach(const runtime::Image& designImage, runtime::Kernel& simulationKernel,
                const SourceManager* sourceManager = nullptr);

    /// Unbinds the design. Every handle to it becomes stale, and every callback that named
    /// part of it is dropped, because what it named no longer exists.
    void detach();

    /// Raises `cbStartOfSimulation`. Only the first call does anything.
    void startOfSimulation();

    /// Raises `cbEndOfSimulation`. Only the first call does anything (section 9.5).
    void endOfSimulation();

    /// Where `vpi_printf` writes. Null means the standard output.
    void setTranscript(std::ostream* stream) noexcept { transcript = stream; }

    // -----------------------------------------------------------------------
    // runtime::PliHooks
    // -----------------------------------------------------------------------

    void valueChanged(std::uint32_t signalIndex) override;
    [[nodiscard]] runtime::SimTime nextTimedEvent() const override;
    void timeAdvanced(runtime::SimTime time) override;
    void runTimedEvents(runtime::SimTime time) override;

    [[nodiscard]] bool hasReadWrite() const override { return !readWrite.empty(); }

    void runReadWrite() override;

    [[nodiscard]] bool hasReadOnly() const override { return !readOnly.empty(); }

    void runReadOnly() override;

    // -----------------------------------------------------------------------
    // The routines of vpi_user.h, one for one. VpiRoutines.cpp forwards to these.
    // -----------------------------------------------------------------------

    vpiHandle registerCallback(const s_cb_data* data);
    PLI_INT32 removeCallback(vpiHandle handle);
    void getCallbackInfo(vpiHandle handle, s_cb_data* data);
    vpiHandle handleByName(const char* name, vpiHandle scope);
    vpiHandle handleByIndex(vpiHandle object, PLI_INT32 index);
    vpiHandle handle(PLI_INT32 type, vpiHandle reference);
    vpiHandle iterate(PLI_INT32 type, vpiHandle reference);
    vpiHandle scan(vpiHandle iterator);
    PLI_INT32 get(PLI_INT32 property, vpiHandle object);
    PLI_BYTE8* getString(PLI_INT32 property, vpiHandle object);
    void getValue(vpiHandle object, s_vpi_value* value);
    vpiHandle putValue(vpiHandle object, s_vpi_value* value, const s_vpi_time* time,
                       PLI_INT32 flags);
    void getTime(vpiHandle object, s_vpi_time* time);
    PLI_INT32 compareObjects(vpiHandle first, vpiHandle second);
    PLI_INT32 checkError(s_vpi_error_info* info);
    PLI_INT32 freeObject(vpiHandle object);
    PLI_INT32 getVlogInfo(s_vpi_vlog_info* info);
    PLI_INT32 control(PLI_INT32 operation);
    PLI_INT32 print(std::string_view text);
    PLI_INT32 flush();

    /// Records that `routine` is not supported, for `vpi_chk_error` (section 9.1).
    void unsupported(std::string_view routine);

#ifdef ENZAN_WITH_VHDL
    // -----------------------------------------------------------------------
    // The routines of vhpi_user.h (section 9.7). VhpiRoutines.cpp forwards to these.
    // -----------------------------------------------------------------------

    vhpiHandleT vhpiHandle(vhpiOneToOneT type, vhpiHandleT reference);
    vhpiHandleT vhpiHandleByName(const char* name, vhpiHandleT scope);
    vhpiHandleT vhpiHandleByIndex(vhpiOneToManyT type, vhpiHandleT parent, std::int32_t index);
    vhpiHandleT vhpiIterator(vhpiOneToManyT type, vhpiHandleT reference);
    vhpiHandleT vhpiScan(vhpiHandleT iterator);
    vhpiIntT vhpiGet(vhpiIntPropertyT property, vhpiHandleT object);
    const vhpiCharT* vhpiGetStr(vhpiStrPropertyT property, vhpiHandleT object);
    vhpiPhysT vhpiGetPhys(vhpiPhysPropertyT property, vhpiHandleT object);
    int vhpiGetValue(vhpiHandleT object, vhpiValueT* value);
    int vhpiPutValue(vhpiHandleT object, vhpiValueT* value, vhpiPutValueModeT mode);
    void vhpiGetTime(vhpiTimeT* time, long* cycles);
    vhpiHandleT vhpiRegisterCallback(vhpiCbDataT* data, std::int32_t flags);
    int vhpiRemoveCallback(vhpiHandleT handle);
    int vhpiReleaseHandle(vhpiHandleT handle);
    int vhpiCompareHandles(vhpiHandleT first, vhpiHandleT second);
    int vhpiCheckError(vhpiErrorInfoT* info);
    int vhpiControl(vhpiSimControlT command);
    int vhpiPrint(std::string_view text);
#endif

    /// Forgets the last error. Every routine but `vpi_chk_error` starts with this, because
    /// the standard defines that routine as reporting on the call before it.
    void clearError() noexcept { errorLevel = 0; }

private:
    struct Object;
    struct Scope;
    struct Callback;

    /// What a design handle names, which is also what it is interned under.
    enum class TargetKind : std::uint8_t { Scope, Signal, Bit, Element, Parameter, Constant };

    struct Target {
        TargetKind kind = TargetKind::Scope;

        /// The scope, or the instance owning the signal or parameter.
        std::uint32_t first = 0;

        /// The object or parameter index; or, for a constant, its value.
        std::int64_t second = 0;

        /// The bit a bit-select selects, or an element's position among the elements at its
        /// depth, counted from the left of the outermost dimension.
        std::uint32_t bit = 0;

        /// For an element, how many unpacked dimensions have been indexed to reach it.
        std::uint8_t depth = 0;

        friend auto operator<=>(const Target&, const Target&) = default;
    };

    /// Phases in which registering a callback means "the next occurrence of this phase".
    enum class Phase : std::uint8_t { None, ReadWrite, ReadOnly };

    // -- the design model --------------------------------------------------

    void buildScopes();

    /// The scope an object of `moduleScope`'s instance is in, creating the generate blocks its
    /// name says it is inside.
    std::uint32_t scopeFor(std::uint32_t moduleScope, std::string_view objectName);

    vpiHandle intern(const Target& target);
    [[nodiscard]] Object* resolve(vpiHandle handle, std::string_view routine);
    /// The value a signal or a bit-select of one holds now.
    [[nodiscard]] eir::BitVector readSignal(const Object& object) const;

    // -- callbacks ----------------------------------------------------------

    void fire(Callback& callback);

    /// Marks a callback as finished with and takes it out of the application's reach. It is
    /// freed by the next sweep, once no dispatch can still be holding it.
    void retire(Callback& callback);

    /// Fires every callback in `pending` once and empties it. A repetitive VHPI callback goes
    /// to `rearm`, for its next occurrence, rather than being retired.
    void runOnce(std::vector<Callback*>& pending, std::vector<Callback*>* rearm = nullptr);

    /// Drops retired callbacks from every list and frees them. Only ever called when no
    /// callback is running.
    void sweepNow();

    void fail(PLI_INT32 level, std::string message);

#ifdef ENZAN_WITH_VHDL
    // -- VHPI ---------------------------------------------------------------
    // A VHPI handle is made for each request and freed when the application releases it, as
    // VHPI has it; what it stands for is in the fields of Object that Vhpi.cpp fills.

    [[nodiscard]] Object* vhpiResolve(vhpiHandleT handle, std::string_view routine);
    vhpiHandleT vhpiKeep(std::unique_ptr<Object> object);
    [[nodiscard]] std::unique_ptr<Object> vhpiRegion(std::uint32_t scopeIndex) const;
    [[nodiscard]] std::unique_ptr<Object>
    vhpiDeclared(std::uint32_t instanceIndex, std::uint32_t objectIndex, bool isGeneric) const;
    [[nodiscard]] std::unique_ptr<Object> vhpiElement(const Object& parent,
                                                      std::uint64_t flatIndex) const;
    [[nodiscard]] std::unique_ptr<Object> vhpiField(const Object& parent, std::size_t field) const;
    [[nodiscard]] std::unique_ptr<Object> vhpiTypeHandle(const Object& of, std::uint32_t type,
                                                         bool base) const;
    [[nodiscard]] std::vector<std::unique_ptr<Object>> vhpiChildren(vhpiOneToManyT type,
                                                                    const Object& reference);
    [[nodiscard]] const eir::VhdlType* vhdlTypeOf(const Object& object) const;
    [[nodiscard]] const eir::ModuleDefinition& definitionOf(std::uint32_t instanceIndex) const;
    [[nodiscard]] eir::BitVector vhpiBits(const Object& object) const;
    bool vhpiWrite(const Object& object, const eir::BitVector& bits, vhpiPutValueModeT mode);
    int vhpiFormat(const Object& object, const eir::BitVector& bits, vhpiValueT& value);
    void vhpiFire(Callback& callback);

    /// What `vhpi_get_str` last returned, kept until it is called again as the standard says.
    std::string vhpiString;
#endif

    [[nodiscard]] std::ostream& transcriptStream() const;

    DiagnosticEngine& diagnostics;
    std::ostream* transcript = nullptr;

    std::vector<std::string> arguments;
    std::vector<char*> argumentPointers;
    std::string product = "Enzan";
    std::string versionText;
    unsigned loadedLibraries = 0;

    const runtime::Image* image = nullptr;
    runtime::Kernel* kernel = nullptr;
    const SourceManager* sources = nullptr;

    std::vector<Scope> scopes;
    std::vector<std::uint32_t> moduleScopeOfInstance;

    /// The scope each object and parameter of each instance is in, or none for one an
    /// application cannot see.
    std::vector<std::vector<std::uint32_t>> objectScopes;
    std::vector<std::vector<std::uint32_t>> parameterScopes;

    /// Every name `vpi_handle_by_name` resolves. Ordered so that nothing about it depends on
    /// hashing (section 3.8).
    std::map<std::string, Target, std::less<>> byFullName;

    /// Every object an application holds a handle to, owned here. The key is the handle, so
    /// that checking one before use is a lookup rather than a dereference (section 9.2).
    std::map<const void*, std::unique_ptr<Object>> objects;
    std::map<Target, Object*> interned;

    // Callbacks, by where they fire. Each list holds them in registration order (3.8).
    std::vector<std::vector<Callback*>> valueChangeWatchers;
    std::multimap<runtime::SimTime, Callback*> timedCallbacks;
    std::vector<Callback*> readWrite;
    std::vector<Callback*> readOnly;
    std::vector<Callback*> nextSimTime;
    std::vector<Callback*> startCallbacks;
    std::vector<Callback*> endCallbacks;

    /// Registered during read-only, to run once time has moved (section 9.3).
    std::vector<Callback*> deferredReadWrite;
    std::vector<Callback*> deferredReadOnly;

    /// Writes with a delay, due at the start of their slot (section 9.4).
    struct DelayedWrite {
        std::uint32_t signal = 0;
        eir::BitVector value;

        /// Where in the signal the value goes, for a write through a bit-select.
        std::uint32_t lowBit = 0;
        bool partial = false;
    };

    std::multimap<runtime::SimTime, DelayedWrite> delayedWrites;

    Phase phase = Phase::None;
    unsigned dispatchDepth = 0;
    bool sweepNeeded = false;
    bool started = false;
    bool ended = false;

    /// `vpi_control(vpiFinish)` before a design was attached, applied to the next one that is.
    bool finishBeforeAttach = false;

    /// Callbacks that have fired or been removed, freed once no dispatch can still hold them.
    std::vector<std::unique_ptr<Object>> retired;

    ValueStorage values;

    PLI_INT32 errorLevel = 0;
    std::string errorMessage;
    std::string errorCode;
    std::string errorFile;
};

/// Does nothing, and is defined beside the `vpi_*` routines so that calling it keeps them.
///
/// Nothing in Enzan calls a routine — applications do — and a linker includes an archive
/// member only when something references it. Without this the routines would be left out of
/// the `enzan` executable, and every application would fail to load over a routine that was
/// never linked rather than one that does not exist.
void keepRoutinesLinked() noexcept;

#ifdef ENZAN_WITH_VHDL
/// The same for the `vhpi_*` routines, which are defined beside it in VhpiRoutines.cpp.
void keepVhpiRoutinesLinked() noexcept;
#endif

} // namespace enzan::pli
