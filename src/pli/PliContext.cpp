// SPDX-License-Identifier: GPL-3.0-or-later

#include "pli/PliContext.h"
#include "pli/PliModel.h"

#include "common/Assert.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "common/Version.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "elab/ElaboratedDesign.h"
#include "runtime/Image.h"

#include "sv_vpi_user.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <iostream>
#include <utility>

#include <dlfcn.h>

namespace enzan::pli {

namespace {

/// The one context the routines act on. There is at most one per process, for the reason there
/// is one session per process; see the class comment.
PliContext* currentContext = nullptr;

/// Whether two names are one in any case, as VHDL compares them.
bool equalsIgnoringCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a))
               == std::tolower(static_cast<unsigned char>(b));
    });
}

/// Splits a hierarchical name at the dots that separate scopes, leaving the ones inside an
/// index or an escaped identifier alone: `gen[0].inner.q` is three names, `\weird.name\` one.
std::vector<std::string_view> splitName(std::string_view name) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    unsigned depth = 0;
    bool escaped = false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        // An escaped identifier holds dots of its own: SystemVerilog's, `\a.b `, runs to the
        // space that ends it, and VHDL's extended one, `\a.b\`, to the backslash that closes it,
        // a doubled one standing for a backslash.
        if (escaped) {
            if (name[index] == ' ') {
                escaped = false;
            } else if (name[index] == '\\') {
                if (index + 1 < name.size() && name[index + 1] == '\\') {
                    ++index;
                } else {
                    escaped = false;
                }
            }
            continue;
        }
        if (name[index] == '\\' && index == start) {
            escaped = true;
            continue;
        }
        if (name[index] == '[') ++depth;
        if (name[index] == ']' && depth > 0) --depth;
        if (name[index] == '.' && depth == 0) {
            parts.push_back(name.substr(start, index - start));
            start = index + 1;
        }
    }
    parts.push_back(name.substr(start));
    return parts;
}

/// Rewrites escaped identifiers to the names they escape: `\a.b ` names the identifier `a.b`.
///
/// The backslash and the space that ends an escaped identifier are how the language writes it
/// and not part of the name, which is what an application looking an object up has in hand.
std::string unescape(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    bool escaped = false;
    for (const char character : name) {
        if (!escaped && character == '\\' && (result.empty() || result.back() == '.')) {
            escaped = true;
            continue;
        }
        if (escaped && character == ' ') {
            escaped = false;
            continue;
        }
        result.push_back(character);
    }
    return result;
}

/// The name `vpi_get_str(vpiType, ...)` gives a type.
const char* typeName(PLI_INT32 type) {
    switch (type) {
    case vpiModule: return "vpiModule";
    case vpiGenScope: return "vpiGenScope";
    case vpiNet: return "vpiNet";
    case vpiReg: return "vpiReg";
    case vpiBitVar: return "vpiBitVar";
    case vpiNetBit: return "vpiNetBit";
    case vpiRegBit: return "vpiRegBit";
    case vpiRegArray: return "vpiRegArray";
    case vpiNetArray: return "vpiNetArray";
    case vpiParameter: return "vpiParameter";
    case vpiConstant: return "vpiConstant";
    case vpiIterator: return "vpiIterator";
    case vpiCallback: return "vpiCallback";
    default: return "vpiUndefined";
    }
}

/// Simulated time in the form a callback or `vpi_get_time` reports it.
void fillTime(s_vpi_time& time, runtime::SimTime now) {
    switch (time.type) {
    case vpiSimTime:
        time.high = static_cast<PLI_UINT32>(now >> 32U);
        time.low = static_cast<PLI_UINT32>(now);
        return;
    case vpiScaledRealTime:
        // Enzan counts every delay in one unit, so a design's time unit and its precision are
        // the same unit and scaling is the identity.
        time.real = static_cast<double>(now);
        return;
    default: return;
    }
}

/// A delay or an absolute time an application gave, in the kernel's units, or nothing when
/// the time is not one that can be.
std::optional<runtime::SimTime> ticksOf(const s_vpi_time& time) {
    switch (time.type) {
    case vpiSimTime:
        return (static_cast<runtime::SimTime>(time.high) << 32U)
               | static_cast<runtime::SimTime>(time.low);
    case vpiScaledRealTime:
        if (!std::isfinite(time.real) || time.real < 0) return std::nullopt;
        return static_cast<runtime::SimTime>(std::llround(time.real));
    default: return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The object model
// ---------------------------------------------------------------------------

PliContext::PliContext(DiagnosticEngine& engine, std::vector<std::string> commandLine)
    : diagnostics(engine), arguments(std::move(commandLine)), versionText(enzan::version()) {
    ENZAN_ALWAYS_ASSERT(currentContext == nullptr, "there is one VPI context per process");
    currentContext = this;

    argumentPointers.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) argumentPointers.push_back(argument.data());
    argumentPointers.push_back(nullptr);

    keepRoutinesLinked();
#ifdef ENZAN_WITH_VHDL
    keepVhpiRoutinesLinked();
#endif
}

PliContext::~PliContext() {
    detach();
    currentContext = nullptr;
}

PliContext* PliContext::current() noexcept {
    return currentContext;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool PliContext::load(std::string_view specification, Interface which) {
    const std::string_view interfaceName = which == Interface::Vpi ? "VPI" : "VHPI";
    // `library:entry`, where the entry is an identifier. A colon followed by a path is part of
    // the path, so a library in a directory whose name has a colon in it still loads.
    std::string library(specification);
    std::string entry;
    const std::size_t colon = specification.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view candidate = specification.substr(colon + 1);
        const bool identifier =
            !candidate.empty() && std::ranges::all_of(candidate, [](char character) {
                return character == '_' || (character >= 'a' && character <= 'z')
                       || (character >= 'A' && character <= 'Z')
                       || (character >= '0' && character <= '9');
            });
        if (identifier) {
            library = std::string(specification.substr(0, colon));
            entry = std::string(candidate);
        }
    }

    // Now, so that a routine the application needs and Enzan lacks is named here rather than
    // being found mid-run; global, so that libraries it loads in turn can resolve against it.
    // The handle is never closed: an application's code stays reachable through the callbacks
    // it registered for the whole life of the process.
    ::dlerror();
    void* opened = ::dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (opened == nullptr) {
        const char* reason = ::dlerror();
        diagnostics.report(DiagCode::VpiLibraryUnloadable, SourceLocation(), interfaceName, library,
                           reason != nullptr ? reason : "the reason was not reported");
        return false;
    }

    // Counted once something in it is started, and not merely opened: a library whose entry
    // is missing was reported and never ran, and is not an application the session holds.
    if (!entry.empty()) {
        void* symbol = ::dlsym(opened, entry.c_str());
        if (symbol == nullptr) {
            diagnostics.report(DiagCode::VpiEntryNotFound, SourceLocation(), interfaceName, library,
                               entry);
            return false;
        }
        ++loadedLibraries;
        reinterpret_cast<void (*)()>(symbol)();
        return true;
    }

    const char* table = which == Interface::Vpi ? "vlog_startup_routines" : "vhpi_startup_routines";
    const void* routines = ::dlsym(opened, table);
    if (routines == nullptr) {
        diagnostics.report(DiagCode::VpiNoStartupRoutines, SourceLocation(), interfaceName, library,
                           table, which == Interface::Vpi ? "vpi" : "vhpi");
        return false;
    }

    ++loadedLibraries;
    auto* const* startup = reinterpret_cast<void (*const*)()>(routines);
    for (std::size_t index = 0; startup[index] != nullptr; ++index) startup[index]();
    return true;
}

// ---------------------------------------------------------------------------
// Binding a design
// ---------------------------------------------------------------------------

void PliContext::attach(const runtime::Image& designImage, runtime::Kernel& simulationKernel,
                        const SourceManager* sourceManager) {
    detach();

    image = &designImage;
    kernel = &simulationKernel;
    sources = sourceManager;

    buildScopes();
    valueChangeWatchers.assign(image->signals().size(), {});
    kernel->setPliHooks(this);

    // A finish asked for before there was anything to finish — by a startup routine that
    // found nothing to do — still ends the run. That run, and not every one after it.
    if (finishBeforeAttach) {
        kernel->requestFinish();
        finishBeforeAttach = false;
    }
}

void PliContext::detach() {
    if (kernel != nullptr) kernel->setPliHooks(nullptr);

    // Every callback that belongs to this simulation goes with it, and every handle to its
    // design: one kept would name a slot in a simulation that no longer exists. Only the start
    // and end of simulation outlive it, because they are about the process rather than about
    // any one elaboration.
    const auto retireAll = [this](const std::vector<Callback*>& list) {
        for (Callback* callback : list) retire(*callback);
    };
    for (const std::vector<Callback*>& watchers : valueChangeWatchers) retireAll(watchers);
    for (const auto& [time, callback] : timedCallbacks) retire(*callback);
    retireAll(readWrite);
    retireAll(readOnly);
    retireAll(deferredReadWrite);
    retireAll(deferredReadOnly);
    retireAll(nextSimTime);
    delayedWrites.clear();
    sweepNow();
    valueChangeWatchers.clear();

    std::erase_if(objects,
                  [](const auto& entry) { return entry.second->kind != Object::Kind::Callback; });
    interned.clear();

    scopes.clear();
    moduleScopeOfInstance.clear();
    objectScopes.clear();
    parameterScopes.clear();
    byFullName.clear();

    image = nullptr;
    kernel = nullptr;
    sources = nullptr;
}

void PliContext::buildScopes() {
    const elab::ElaboratedDesign& elaborated = image->elaboratedDesign();
    const eir::Design& design = image->analysedDesign();

    moduleScopeOfInstance.assign(elaborated.instances().size(), NoScope);
    objectScopes.assign(elaborated.instances().size(), {});
    parameterScopes.assign(elaborated.instances().size(), {});

    // Instances come in elaboration order, which puts every parent before its children.
    for (std::uint32_t instanceIndex = 0; instanceIndex < elaborated.instances().size();
         ++instanceIndex) {
        const elab::Instance& instance = elaborated.instances()[instanceIndex];
        const eir::ModuleDefinition& definition = design.module(instance.definitionIndex);

        // An instance inside a generate block is named for the block, as an object is, and is
        // a child of the block rather than of the module. Found first, because finding it may
        // create the block's scope.
        std::uint32_t enclosing = NoScope;
        if (instance.parent != elab::Instance::NoParent)
            enclosing = scopeFor(moduleScopeOfInstance[instance.parent], instance.name);

        const auto scopeIndex = static_cast<std::uint32_t>(scopes.size());
        Scope scope;
        scope.isModule = true;
        scope.isPackage = definition.isPackage;
        scope.vhdl = definition.language == eir::Language::Vhdl;
        scope.instance = instanceIndex;
        scope.name = enclosing != NoScope ? instance.name.substr(scopes[enclosing].prefix.size())
                                          : instance.name;
        scope.fullName = elab::dottedPath(instance.path);
        if (enclosing != NoScope) {
            scope.parent = enclosing;
            scopes[enclosing].children.push_back(scopeIndex);
        }
        if (!scope.vhdl)
            byFullName.emplace(scope.fullName,
                               Target{.kind = TargetKind::Scope, .first = scopeIndex});
        scopes.push_back(std::move(scope));
        moduleScopeOfInstance[instanceIndex] = scopeIndex;

        std::vector<std::uint32_t>& objectScope = objectScopes[instanceIndex];
        objectScope.assign(definition.objects.size(), NoScope);
        for (std::uint32_t objectIndex = 0; objectIndex < definition.objects.size();
             ++objectIndex) {
            const eir::Object& object = definition.objects[objectIndex];
            if (eir::isIntroducedName(object.name)) continue;
            if (object.vhdlType != eir::VhdlType::None)
                scopes[scopeFor(scopeIndex, object.name)].vhdlObjects.push_back(objectIndex);

            // Only what holds a packed value has a slot, and a name with a `$` in it is one a
            // frontend made up and kept out of the user's namespace on purpose. A VHDL signal
            // of a null range holds no value VPI could give, and an array or struct holding reals
            // would show their doubles as bits.
            if (!object.type.isBits() || object.holdsNothing || object.holdsReals) continue;

            const std::uint32_t owner = scopeFor(scopeIndex, object.name);
            objectScope[objectIndex] = owner;
            scopes[owner].signals.push_back(objectIndex);
            if (!scopes[owner].vhdl) {
                byFullName.emplace(
                    scopes[owner].fullName + '.' + object.name.substr(scopes[owner].prefix.size()),
                    Target{
                        .kind = TargetKind::Signal, .first = instanceIndex, .second = objectIndex});
            }
        }

        std::vector<std::uint32_t>& parameterScope = parameterScopes[instanceIndex];
        parameterScope.assign(definition.parameters.size(), NoScope);
        for (std::uint32_t parameterIndex = 0; parameterIndex < definition.parameters.size();
             ++parameterIndex) {
            const eir::Parameter& parameter = definition.parameters[parameterIndex];
            const std::uint32_t owner = scopeFor(scopeIndex, parameter.name);
            parameterScope[parameterIndex] = owner;
            scopes[owner].parameters.push_back(parameterIndex);
            if (scopes[owner].vhdl) continue;
            byFullName.emplace(scopes[owner].fullName + '.'
                                   + parameter.name.substr(scopes[owner].prefix.size()),
                               Target{.kind = TargetKind::Parameter,
                                      .first = instanceIndex,
                                      .second = parameterIndex});
        }
    }
}

std::uint32_t PliContext::scopeFor(std::uint32_t moduleScope, std::string_view objectName) {
    const std::vector<std::string_view> parts = splitName(objectName);

    std::uint32_t current = moduleScope;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        const std::string_view part = parts[index];

        std::uint32_t found = NoScope;
        for (const std::uint32_t child : scopes[current].children) {
            if (!scopes[child].isModule && scopes[child].name == part) {
                found = child;
                break;
            }
        }

        if (found == NoScope) {
            found = static_cast<std::uint32_t>(scopes.size());
            Scope block;
            block.isModule = false;
            block.vhdl = scopes[current].vhdl;
            block.instance = scopes[current].instance;
            block.parent = current;
            block.prefix = scopes[current].prefix + std::string(part) + '.';
            block.name = std::string(part);
            block.fullName = scopes[current].fullName + '.' + std::string(part);
            if (!block.vhdl)
                byFullName.emplace(block.fullName,
                                   Target{.kind = TargetKind::Scope, .first = found});
            scopes[current].children.push_back(found);
            scopes.push_back(std::move(block));
        }
        current = found;
    }
    return current;
}

vpiHandle PliContext::intern(const Target& target) {
    const auto found = interned.find(target);
    if (found != interned.end()) return reinterpret_cast<vpiHandle>(found->second);

    auto object = std::make_unique<Object>();
    object->target = target;

    const auto sourcePath = [this](SourceLocation location) {
        if (sources == nullptr || !location.valid()) return std::string();
        return std::string(sources->bufferName(location.buffer()));
    };
    const auto sourceLine = [this](SourceLocation location) -> std::uint32_t {
        if (sources == nullptr || !location.valid()) return 0;
        return sources->lineNumber(location);
    };

    switch (target.kind) {
    case TargetKind::Scope: {
        const Scope& scope = scopes[target.first];
        const eir::ModuleDefinition& definition = image->analysedDesign().module(
            image->elaboratedDesign().instances()[scope.instance].definitionIndex);
        object->kind = Object::Kind::Scope;
        object->type = scope.isModule ? vpiModule : vpiGenScope;
        if (scope.vhdl) object->type = vpiUnknown;
        object->scope = target.first;
        object->name = scope.name;
        object->fullName = scope.fullName;
        if (scope.isModule) {
            object->definitionName = definition.sourceName;
            object->file = sourcePath(definition.location);
            object->line = sourceLine(definition.location);
        }
        break;
    }

    case TargetKind::Signal: {
        const auto instanceIndex = target.first;
        const auto objectIndex = static_cast<std::uint32_t>(target.second);
        const elab::Instance& instance = image->elaboratedDesign().instances()[instanceIndex];
        const eir::Object& declared =
            image->analysedDesign().module(instance.definitionIndex).objects[objectIndex];
        const std::uint32_t owner = objectScopes[instanceIndex][objectIndex];

        object->kind = Object::Kind::Signal;
        if (declared.kind == eir::ObjectKind::Net)
            object->type = vpiNet;
        else
            object->type = declared.type.isFourState() ? vpiReg : vpiBitVar;
        // An unpacked array is an array of its elements, which are arrays in turn while
        // dimensions remain.
        if (!declared.unpackedDimensions.empty()) {
            object->type = declared.kind == eir::ObjectKind::Net ? vpiNetArray : vpiRegArray;
            object->isArray = true;
            object->dimensions = declared.unpackedDimensions;
            object->left = declared.unpackedDimensions.front().left;
            object->right = declared.unpackedDimensions.front().right;
            object->elementWidth = declared.elementWidth;
        }
        object->scope = owner;
        object->signal = instance.objectSlots[objectIndex];
        object->width = declared.type.bitWidth();
        object->isSigned = declared.type.isSigned();
        object->name = declared.name.substr(scopes[owner].prefix.size());
        object->fullName = scopes[owner].fullName + '.' + object->name;
        object->file = sourcePath(declared.location);
        object->line = sourceLine(declared.location);
        break;
    }

    case TargetKind::Bit: {
        // A bit of a signal, named and typed from the signal it selects from.
        const Object& vector = *reinterpret_cast<const Object*>(
            intern({.kind = TargetKind::Signal, .first = target.first, .second = target.second}));
        object->kind = Object::Kind::Signal;
        object->type = vector.type == vpiNet ? vpiNetBit : vpiRegBit;
        object->scope = vector.scope;
        object->signal = vector.signal;
        object->width = 1;
        object->isWindow = true;
        object->lowBit = target.bit;
        object->name = std::format("{}[{}]", vector.name, target.bit);
        object->fullName = std::format("{}[{}]", vector.fullName, target.bit);
        object->file = vector.file;
        object->line = vector.line;
        break;
    }

    case TargetKind::Element: {
        // An element of an array, reached through `depth` indices: a window of the array's
        // storage, the left element highest. It is an array itself while dimensions remain
        // below it.
        const Object& whole = *reinterpret_cast<const Object*>(
            intern({.kind = TargetKind::Signal, .first = target.first, .second = target.second}));
        const std::uint8_t depth = target.depth;
        const std::uint32_t parentSize = whole.dimensions[depth - 1U].size();
        const Object& parent = *reinterpret_cast<const Object*>(
            depth == 1
                ? intern(
                      {.kind = TargetKind::Signal, .first = target.first, .second = target.second})
                : intern({.kind = TargetKind::Element,
                          .first = target.first,
                          .second = target.second,
                          .bit = target.bit / parentSize,
                          .depth = static_cast<std::uint8_t>(depth - 1U)}));

        std::uint32_t blocks = 1;
        for (std::uint8_t level = 0; level < depth; ++level)
            blocks *= whole.dimensions[level].size();
        std::uint32_t blockWidth = whole.elementWidth;
        for (std::size_t level = depth; level < whole.dimensions.size(); ++level)
            blockWidth *= whole.dimensions[level].size();

        const eir::ArrayDimension& selectedFrom = whole.dimensions[depth - 1U];
        const std::uint32_t offset = target.bit % parentSize;

        object->kind = Object::Kind::Signal;
        object->scope = whole.scope;
        object->signal = whole.signal;
        object->width = blockWidth;
        object->isWindow = true;
        object->isElement = true;
        object->lowBit = (blocks - 1 - target.bit) * blockWidth;
        object->index = selectedFrom.left <= selectedFrom.right
                            ? std::int64_t{selectedFrom.left} + offset
                            : std::int64_t{selectedFrom.left} - offset;
        object->elementWidth = whole.elementWidth;
        object->dimensions.assign(whole.dimensions.begin() + depth, whole.dimensions.end());
        if (object->dimensions.empty()) {
            const elab::Instance& instance = image->elaboratedDesign().instances()[target.first];
            const eir::Object& declared = image->analysedDesign()
                                              .module(instance.definitionIndex)
                                              .objects[static_cast<std::size_t>(target.second)];
            if (declared.kind == eir::ObjectKind::Net)
                object->type = vpiNet;
            else
                object->type = declared.type.isFourState() ? vpiReg : vpiBitVar;
            object->isSigned = declared.elementSigned;
        } else {
            object->type = whole.type;
            object->isArray = true;
            object->left = object->dimensions.front().left;
            object->right = object->dimensions.front().right;
        }
        object->name = std::format("{}[{}]", parent.name, object->index);
        object->fullName = std::format("{}[{}]", parent.fullName, object->index);
        object->file = whole.file;
        object->line = whole.line;
        break;
    }

    case TargetKind::Parameter: {
        const auto instanceIndex = target.first;
        const auto parameterIndex = static_cast<std::uint32_t>(target.second);
        const elab::Instance& instance = image->elaboratedDesign().instances()[instanceIndex];
        const eir::Parameter& parameter =
            image->analysedDesign().module(instance.definitionIndex).parameters[parameterIndex];
        const std::uint32_t owner = parameterScopes[instanceIndex][parameterIndex];

        object->kind = Object::Kind::Parameter;
        object->type = vpiParameter;
        object->scope = owner;
        object->width = parameter.value.width();
        object->name = parameter.name.substr(scopes[owner].prefix.size());
        object->fullName = scopes[owner].fullName + '.' + object->name;
        object->file = sourcePath(parameter.location);
        object->line = sourceLine(parameter.location);
        break;
    }

    case TargetKind::Constant:
        object->kind = Object::Kind::Constant;
        object->type = vpiConstant;
        object->width = 32;
        object->isSigned = true;
        break;
    }

    Object* raw = object.get();
    objects.emplace(raw, std::move(object));
    interned.emplace(target, raw);
    return reinterpret_cast<vpiHandle>(raw);
}

PliContext::Object* PliContext::resolve(vpiHandle handle, std::string_view routine) {
    const auto found = objects.find(handle);
    if (found != objects.end()) return found->second.get();

    fail(vpiError,
         std::format("{}: the handle is not one Enzan issued, or it has been freed", routine));
    return nullptr;
}

eir::BitVector PliContext::readSignal(const Object& object) const {
    const runtime::Signal& signal = image->signals()[object.signal];
    const eir::ConstBitRef whole = kernel->arena().at(signal.wordOffset, signal.bitWidth);
    if (!object.isWindow) return eir::BitVector::from(whole);
    eir::BitVector window(object.width);
    eir::bitops::partSelectAt(window.mutableRef(), whole, object.lowBit);
    return window;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

void PliContext::fail(PLI_INT32 level, std::string message) {
    errorLevel = level;
    errorMessage = std::move(message);
}

void PliContext::unsupported(std::string_view routine) {
    fail(vpiError, std::format("{} is not supported by Enzan", routine));
}

PLI_INT32 PliContext::checkError(s_vpi_error_info* info) {
    if (errorLevel == 0) return 0;
    if (info != nullptr) {
        info->state = vpiRun;
        info->level = errorLevel;
        info->message = errorMessage.data();
        info->product = product.data();
        info->code = errorCode.data();
        info->file = errorFile.data();
        info->line = 0;
    }
    return errorLevel;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

vpiHandle PliContext::handleByName(const char* name, vpiHandle scope) {
    if (name == nullptr) {
        fail(vpiError, "vpi_handle_by_name: the name is a null pointer");
        return nullptr;
    }
    if (image == nullptr) return nullptr;

    std::string wanted = unescape(name);
    if (scope != nullptr) {
        const Object* within = resolve(scope, "vpi_handle_by_name");
        if (within == nullptr) return nullptr;
        if (within->kind != Object::Kind::Scope) {
            fail(vpiError, "vpi_handle_by_name: the scope is not a module or a generate block");
            return nullptr;
        }
        wanted = within->fullName + '.' + wanted;
    }

    if (const auto found = byFullName.find(wanted); found != byFullName.end())
        return intern(found->second);

    // A name another interface made, whose start is the path of a VHDL scope in that interface's
    // form, `VHDL_TOP.I_SV.sig` as cocotb makes it from VHPI's names: the VHDL scopes matched in
    // any case, as VHDL names them, and what follows them as VPI names it.
    const std::vector<std::string_view> parts = splitName(wanted);
    std::uint32_t current = NoScope;
    std::size_t next = 0;
    for (; next < parts.size(); ++next) {
        std::uint32_t found = NoScope;
        for (std::uint32_t index = 0; index < scopes.size() && found == NoScope; ++index) {
            const Scope& candidate = scopes[index];
            if (candidate.parent != current || candidate.isPackage) continue;
            if (candidate.vhdl ? equalsIgnoringCase(candidate.name, parts[next])
                               : candidate.name == parts[next])
                found = index;
        }
        if (found == NoScope || !scopes[found].vhdl) {
            if (found != NoScope) current = found;
            if (found != NoScope) ++next;
            break;
        }
        current = found;
    }
    if (current == NoScope || scopes[current].vhdl) return nullptr;
    std::string canonical = scopes[current].fullName;
    for (; next < parts.size(); ++next) canonical += '.' + std::string(parts[next]);
    const auto crossing = byFullName.find(canonical);
    if (crossing == byFullName.end()) return nullptr;
    return intern(crossing->second);
}

vpiHandle PliContext::handleByIndex(vpiHandle object, PLI_INT32 index) {
    const Object* found = resolve(object, "vpi_handle_by_index");
    if (found == nullptr) return nullptr;

    // An element of an array, by its declared index.
    if (found->isArray) {
        if (index < std::min(found->left, found->right)
            || index > std::max(found->left, found->right)) {
            fail(vpiError, std::format("vpi_handle_by_index: {} is outside [{}:{}]", index,
                                       found->left, found->right));
            return nullptr;
        }
        const auto offset = static_cast<std::uint32_t>(
            found->left <= found->right ? index - found->left : found->left - index);
        return intern({.kind = TargetKind::Element,
                       .first = found->target.first,
                       .second = found->target.second,
                       .bit = (found->target.bit * found->dimensions.front().size()) + offset,
                       .depth = static_cast<std::uint8_t>(found->target.depth + 1U)});
    }

    // A bit of a vector. Its range is `[width-1:0]`, so an index is a bit position;
    // anything else Enzan has no elements of.
    if (found->kind != Object::Kind::Signal || found->isWindow || found->width < 2) {
        fail(vpiError, std::format("vpi_handle_by_index: a {} has no elements to select",
                                   typeName(found->type)));
        return nullptr;
    }
    if (index < 0 || std::cmp_greater_equal(index, found->width)) {
        fail(vpiError,
             std::format("vpi_handle_by_index: {} is outside [{}:0]", index, found->width - 1));
        return nullptr;
    }
    return intern({.kind = TargetKind::Bit,
                   .first = found->target.first,
                   .second = found->target.second,
                   .bit = static_cast<std::uint32_t>(index)});
}

vpiHandle PliContext::handle(PLI_INT32 type, vpiHandle reference) {
    if (reference == nullptr) return nullptr;
    const Object* object = resolve(reference, "vpi_handle");
    if (object == nullptr) return nullptr;

    switch (type) {
    case vpiParent:
    case vpiScope:
        // A bit-select's parent is the vector it selects from, and an element's its array.
        if (type == vpiParent && object->isWindow) {
            if (object->target.depth > 1) {
                const Object& whole =
                    *reinterpret_cast<const Object*>(intern({.kind = TargetKind::Signal,
                                                             .first = object->target.first,
                                                             .second = object->target.second}));
                return intern(
                    {.kind = TargetKind::Element,
                     .first = object->target.first,
                     .second = object->target.second,
                     .bit = object->target.bit / whole.dimensions[object->target.depth - 1U].size(),
                     .depth = static_cast<std::uint8_t>(object->target.depth - 1U)});
            }
            return intern({.kind = TargetKind::Signal,
                           .first = object->target.first,
                           .second = object->target.second});
        }
        if (object->kind == Object::Kind::Scope) {
            const std::uint32_t parent = scopes[object->scope].parent;
            return parent == NoScope ? nullptr
                                     : intern({.kind = TargetKind::Scope, .first = parent});
        }
        if (object->kind == Object::Kind::Signal || object->kind == Object::Kind::Parameter)
            return intern({.kind = TargetKind::Scope, .first = object->scope});
        return nullptr;

    case vpiModule: {
        if (object->kind != Object::Kind::Scope && object->kind != Object::Kind::Signal
            && object->kind != Object::Kind::Parameter) {
            return nullptr;
        }
        // The module an object is in: through any generate blocks, and for a module, the one
        // that instantiates it.
        std::uint32_t current = object->scope;
        if (object->kind == Object::Kind::Scope && scopes[current].isModule)
            current = scopes[current].parent;
        while (current != NoScope && !scopes[current].isModule) current = scopes[current].parent;
        return current == NoScope ? nullptr : intern({.kind = TargetKind::Scope, .first = current});
    }

    case vpiIndex:
        if (object->isElement)
            return intern({.kind = TargetKind::Constant, .first = 0, .second = object->index});
        if (!object->isWindow) return nullptr;
        return intern({.kind = TargetKind::Constant, .first = 0, .second = object->lowBit});

    case vpiLeftRange:
    case vpiRightRange:
        if (object->isArray) {
            return intern({.kind = TargetKind::Constant,
                           .first = 0,
                           .second = type == vpiLeftRange ? object->left : object->right});
        }
        // EIR does not keep declared bounds, so a vector is described as
        // `[width-1:0]`, and a scalar has no range at all.
        if ((object->kind != Object::Kind::Signal && object->kind != Object::Kind::Parameter)
            || object->width < 2) {
            return nullptr;
        }
        return intern({.kind = TargetKind::Constant,
                       .first = 0,
                       .second = type == vpiLeftRange ? object->width - 1 : 0});

    default: return nullptr;
    }
}

vpiHandle PliContext::iterate(PLI_INT32 type, vpiHandle reference) {
    std::vector<vpiHandle> items;

    const Object* object = nullptr;
    if (reference != nullptr) {
        object = resolve(reference, "vpi_iterate");
        if (object == nullptr) return nullptr;
    }

    if (object == nullptr) {
        // From the root, every top-level instance; Enzan's only kind of instance is a module,
        // so asking for instances in general gets the same answer.
        if ((type == vpiModule || type == vpiInstance) && image != nullptr) {
            for (std::uint32_t scope = 0; scope < scopes.size(); ++scope) {
                if (scopes[scope].isModule && !scopes[scope].isPackage && !scopes[scope].vhdl
                    && scopes[scope].parent == NoScope)
                    items.push_back(intern({.kind = TargetKind::Scope, .first = scope}));
            }
        }
    } else if (object->kind == Object::Kind::Signal && object->isArray) {
        // The elements of an array, left to right as its range runs: vectors, or arrays while
        // dimensions remain below it.
        const bool leaves = object->dimensions.size() == 1;
        const bool wanted =
            leaves ? type == vpiReg || type == vpiNet || type == vpiBitVar || type == vpiMemoryWord
                   : type == vpiRegArray || type == vpiNetArray;
        if (wanted) {
            const std::uint32_t count = object->dimensions.front().size();
            for (std::uint32_t offset = 0; offset < count; ++offset) {
                items.push_back(
                    intern({.kind = TargetKind::Element,
                            .first = object->target.first,
                            .second = object->target.second,
                            .bit = (object->target.bit * count) + offset,
                            .depth = static_cast<std::uint8_t>(object->target.depth + 1U)}));
            }
        }
    } else if (object->kind == Object::Kind::Signal) {
        // The bits of a vector, left to right as its range `[width-1:0]` runs.
        const bool bits = type == vpiBit || type == vpiNetBit || type == vpiRegBit;
        if (bits && !object->isWindow && object->width > 1) {
            for (std::uint32_t bit = object->width; bit-- > 0;) {
                items.push_back(intern({.kind = TargetKind::Bit,
                                        .first = object->target.first,
                                        .second = object->target.second,
                                        .bit = bit}));
            }
        }
    } else if (object->kind == Object::Kind::Scope && !scopes[object->scope].vhdl) {
        const Scope& scope = scopes[object->scope];
        const auto signalType = [&](std::uint32_t objectIndex) {
            const eir::Object& declared =
                image->analysedDesign()
                    .module(image->elaboratedDesign().instances()[scope.instance].definitionIndex)
                    .objects[objectIndex];
            if (!declared.unpackedDimensions.empty())
                return declared.kind == eir::ObjectKind::Net ? vpiNetArray : vpiRegArray;
            if (declared.kind == eir::ObjectKind::Net) return vpiNet;
            return declared.type.isFourState() ? vpiReg : vpiBitVar;
        };

        switch (type) {
        case vpiModule:
        case vpiInternalScope:
            for (const std::uint32_t child : scope.children) {
                if (type == vpiModule && !scopes[child].isModule) continue;
                items.push_back(intern({.kind = TargetKind::Scope, .first = child}));
            }
            break;

        case vpiNet:
        case vpiReg:
        case vpiBitVar:
        case vpiRegArray:
        case vpiNetArray:
        case vpiMemory:
        case vpiVariables:
            for (const std::uint32_t objectIndex : scope.signals) {
                const PLI_INT32 actual = signalType(objectIndex);
                // An IEEE 1364 memory is an array of variables.
                bool wanted = actual == type;
                if (type == vpiVariables) wanted = actual != vpiNet && actual != vpiNetArray;
                if (type == vpiMemory) wanted = actual == vpiRegArray;
                if (!wanted) continue;
                items.push_back(intern(
                    {.kind = TargetKind::Signal, .first = scope.instance, .second = objectIndex}));
            }
            break;

        case vpiParameter:
            for (const std::uint32_t parameterIndex : scope.parameters) {
                items.push_back(intern({.kind = TargetKind::Parameter,
                                        .first = scope.instance,
                                        .second = parameterIndex}));
            }
            break;

        default: break;
        }
    }

    // The standard returns no iterator at all when there is nothing to iterate over.
    if (items.empty()) return nullptr;

    auto iterator = std::make_unique<Object>();
    iterator->kind = Object::Kind::Iterator;
    iterator->type = vpiIterator;
    iterator->iteratorType = type;
    iterator->items = std::move(items);

    Object* raw = iterator.get();
    objects.emplace(raw, std::move(iterator));
    return reinterpret_cast<vpiHandle>(raw);
}

vpiHandle PliContext::scan(vpiHandle iterator) {
    Object* object = resolve(iterator, "vpi_scan");
    if (object == nullptr) return nullptr;
    if (object->kind != Object::Kind::Iterator) {
        fail(vpiError, "vpi_scan: the handle is not an iterator");
        return nullptr;
    }

    if (object->position < object->items.size()) return object->items[object->position++];

    // An exhausted iterator is freed by the scan that exhausts it, as the standard says; the
    // application must not free it again, and a handle check catches one that tries.
    objects.erase(iterator);
    return nullptr;
}

PLI_INT32 PliContext::compareObjects(vpiHandle first, vpiHandle second) {
    if (resolve(first, "vpi_compare_objects") == nullptr) return 0;
    if (resolve(second, "vpi_compare_objects") == nullptr) return 0;
    // Design handles are interned, so one object has one handle.
    return first == second ? 1 : 0;
}

PLI_INT32 PliContext::freeObject(vpiHandle object) {
    const Object* found = resolve(object, "vpi_free_object");
    if (found == nullptr) return 0;

    // Only an iterator is owned by the application. A design handle names something that
    // exists for the whole elaboration, and freeing a callback's handle leaves the callback
    // registered, so freeing either changes nothing.
    if (found->kind == Object::Kind::Iterator) objects.erase(object);
    return 1;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

PLI_INT32 PliContext::get(PLI_INT32 property, vpiHandle object) {
    const PLI_INT32 precision = kernel != nullptr ? kernel->timeScale().precisionExponent()
                                                  : runtime::TimeScale().precisionExponent();

    if (object == nullptr) {
        if (property == vpiTimePrecision || property == vpiTimeUnit) return precision;
        fail(vpiError, std::format("vpi_get: property {} needs an object", property));
        return vpiUndefined;
    }

    const Object* found = resolve(object, "vpi_get");
    if (found == nullptr) return vpiUndefined;

    const bool hasValue = found->kind == Object::Kind::Signal
                          || found->kind == Object::Kind::Parameter
                          || found->kind == Object::Kind::Constant;

    switch (property) {
    case vpiType: return found->type;

    case vpiSize:
        // An array's size is how many elements it has (IEEE 1800 section 37.17).
        if (found->isArray) return static_cast<PLI_INT32>(found->dimensions.front().size());
        if (hasValue) return static_cast<PLI_INT32>(found->width);
        break;

    case vpiVector:
        if (hasValue) return found->width > 1 ? 1 : 0;
        break;

    case vpiScalar:
        if (hasValue) return found->width == 1 ? 1 : 0;
        break;

    case vpiSigned:
        if (hasValue) return found->isSigned ? 1 : 0;
        break;

    case vpiTopModule:
        if (found->kind == Object::Kind::Scope)
            return scopes[found->scope].isModule && !scopes[found->scope].isPackage
                           && scopes[found->scope].parent == NoScope
                       ? 1
                       : 0;
        break;

    case vpiTimeUnit:
    case vpiTimePrecision:
        if (found->kind == Object::Kind::Scope) return precision;
        break;

    case vpiNetType:
        if (found->type == vpiNet) return vpiWire;
        break;

    case vpiLocalParam:
        if (found->kind == Object::Kind::Parameter) {
            const elab::Instance& instance =
                image->elaboratedDesign().instances()[found->target.first];
            return image->analysedDesign()
                           .module(instance.definitionIndex)
                           .parameters[static_cast<std::size_t>(found->target.second)]
                           .isLocal
                       ? 1
                       : 0;
        }
        break;

    case vpiConstType:
        if (found->kind == Object::Kind::Parameter) return vpiBinaryConst;
        if (found->kind == Object::Kind::Constant) return vpiDecConst;
        break;

    case vpiLineNo:
        if (found->line != 0) return static_cast<PLI_INT32>(found->line);
        break;

    case vpiIteratorType:
        if (found->kind == Object::Kind::Iterator) return found->iteratorType;
        break;

    case vpiArray: return found->isArray ? 1 : 0;
    case vpiMultiArray: return found->isArray && found->dimensions.size() > 1 ? 1 : 0;
    case vpiIsMemory: return found->isArray && found->type == vpiRegArray ? 1 : 0;

    // Properties of constructs Enzan's subset has none of, whose answer is therefore no.
    case vpiPacked:
    case vpiCellInstance:
    case vpiProtected:
    case vpiIsProtected:
    case vpiAutomatic:
    case vpiExplicitScalared:
    case vpiExplicitVectored:
    case vpiImplicitDecl: return 0;

    default: break;
    }

    fail(vpiError, std::format("vpi_get: property {} is not available for a {}", property,
                               typeName(found->type)));
    return vpiUndefined;
}

PLI_BYTE8* PliContext::getString(PLI_INT32 property, vpiHandle object) {
    const Object* found = resolve(object, "vpi_get_str");
    if (found == nullptr) return nullptr;

    const bool named = found->kind == Object::Kind::Scope || found->kind == Object::Kind::Signal
                       || found->kind == Object::Kind::Parameter;

    // The strings live in the interned object, so they stay valid for as long as its handle
    // does — longer than the standard promises, which is what makes holding two of them safe.
    const auto text = [](const std::string& value) -> PLI_BYTE8* {
        return value.empty() ? nullptr : const_cast<PLI_BYTE8*>(value.c_str());
    };

    switch (property) {
    case vpiType: return const_cast<PLI_BYTE8*>(typeName(found->type));
    case vpiName:
        if (named) return text(found->name);
        break;
    case vpiFullName:
        if (named) return text(found->fullName);
        break;
    case vpiDefName:
        if (found->type == vpiModule) return text(found->definitionName);
        break;
    case vpiFile:
    case vpiDefFile:
        if (named) return text(found->file);
        break;
    default: break;
    }

    fail(vpiError, std::format("vpi_get_str: property {} is not available for a {}", property,
                               typeName(found->type)));
    return nullptr;
}

// ---------------------------------------------------------------------------
// Values and time
// ---------------------------------------------------------------------------

void PliContext::getValue(vpiHandle object, s_vpi_value* value) {
    if (value == nullptr) {
        fail(vpiError, "vpi_get_value: the value is a null pointer");
        return;
    }
    const Object* found = resolve(object, "vpi_get_value");
    if (found == nullptr) return;

    std::string error;
    bool ok = false;
    switch (found->kind) {
    case Object::Kind::Signal:
        if (found->isArray) {
            error = "an array has no value of its own; read its elements";
            break;
        }
        ok = readValue(readSignal(*found).ref(), found->isSigned, *value, values, error);
        break;

    case Object::Kind::Parameter: {
        const elab::Instance& instance = image->elaboratedDesign().instances()[found->target.first];
        const eir::Parameter& parameter =
            image->analysedDesign()
                .module(instance.definitionIndex)
                .parameters[static_cast<std::size_t>(found->target.second)];
        ok = readValue(parameter.value.ref(), false, *value, values, error);
        break;
    }

    case Object::Kind::Constant: {
        const eir::BitVector constant = eir::BitVector::fromInt64(found->target.second, 32);
        ok = readValue(constant.ref(), true, *value, values, error);
        break;
    }

    default: error = std::format("a {} has no value", typeName(found->type)); break;
    }

    if (!ok) fail(vpiError, "vpi_get_value: " + error);
}

vpiHandle PliContext::putValue(vpiHandle object, s_vpi_value* value, const s_vpi_time* time,
                               PLI_INT32 flags) {
    const Object* found = resolve(object, "vpi_put_value");
    if (found == nullptr) return nullptr;
    if (found->kind != Object::Kind::Signal || found->isArray) {
        fail(vpiError, std::format("vpi_put_value: a {} cannot be written{}", typeName(found->type),
                                   found->isArray ? "; write its elements" : ""));
        return nullptr;
    }

    // A read-only phase that could be written would report values that depend on the order
    // its callbacks ran in.
    if (phase == Phase::ReadOnly) {
        fail(vpiError, "vpi_put_value: nothing may be written during cbReadOnlySynch");
        return nullptr;
    }

    if ((flags & vpiReturnEvent) != 0) {
        fail(vpiError, "vpi_put_value: vpiReturnEvent is not supported");
        return nullptr;
    }

    const PLI_INT32 mode =
        flags & ~(vpiReturnEvent | vpiUserAllocFlag | vpiOneValue | vpiPropagateOff);

    // A force holds a whole signal, so it is forced and released whole.
    if ((mode == vpiForceFlag || mode == vpiReleaseFlag) && found->isWindow) {
        fail(vpiError, "vpi_put_value: a bit or an element of a signal cannot be forced or "
                       "released on its own; force the signal");
        return nullptr;
    }

    if (mode == vpiReleaseFlag) {
        kernel->release(found->signal);
        if (value != nullptr) {
            std::string ignored;
            static_cast<void>(
                readValue(readSignal(*found).ref(), found->isSigned, *value, values, ignored));
        }
        return nullptr;
    }

    if (value == nullptr) {
        fail(vpiError, "vpi_put_value: the value is a null pointer");
        return nullptr;
    }

    std::string error;
    const std::optional<eir::BitVector> parsed = parseValue(*value, found->width, error);
    if (!parsed.has_value()) {
        fail(vpiError, "vpi_put_value: " + error);
        return nullptr;
    }

    switch (mode) {
    case 0:
    case vpiNoDelay:
        if (found->isWindow)
            kernel->writeSignalPart(found->signal, parsed->ref(), found->lowBit);
        else
            kernel->deposit(found->signal, parsed->ref());
        return nullptr;

    case vpiInertialDelay:
    case vpiTransportDelay:
    case vpiPureTransportDelay: {
        runtime::SimTime delay = 0;
        if (time != nullptr) {
            const std::optional<runtime::SimTime> ticks = ticksOf(*time);
            if (!ticks.has_value()) {
                fail(vpiError, "vpi_put_value: the delay is not a time Enzan can schedule");
                return nullptr;
            }
            delay = *ticks;
        }

        // No delay is a non-blocking assignment; a delay lands at the start of the slot it reaches.
        if (delay == 0 && found->isWindow) {
            kernel->scheduleNonBlockingPart(found->signal, parsed->ref(), found->lowBit);
        } else if (delay == 0) {
            kernel->scheduleNonBlocking(found->signal, parsed->ref());
        } else {
            delayedWrites.emplace(kernel->time() + delay, DelayedWrite{.signal = found->signal,
                                                                       .value = *parsed,
                                                                       .lowBit = found->lowBit,
                                                                       .partial = found->isWindow});
        }
        return nullptr;
    }

    case vpiForceFlag: kernel->force(found->signal, parsed->ref()); return nullptr;

    default:
        fail(vpiError, std::format("vpi_put_value: flag {} is not supported", mode));
        return nullptr;
    }
}

void PliContext::getTime(vpiHandle object, s_vpi_time* time) {
    if (time == nullptr) {
        fail(vpiError, "vpi_get_time: the time is a null pointer");
        return;
    }
    if (object != nullptr && resolve(object, "vpi_get_time") == nullptr) return;
    fillTime(*time, kernel != nullptr ? kernel->time() : 0);
}

// ---------------------------------------------------------------------------
// The simulator
// ---------------------------------------------------------------------------

PLI_INT32 PliContext::getVlogInfo(s_vpi_vlog_info* info) {
    if (info == nullptr) {
        fail(vpiError, "vpi_get_vlog_info: the information is a null pointer");
        return 0;
    }
    info->argc = static_cast<PLI_INT32>(arguments.size());
    info->argv = argumentPointers.data();
    info->product = product.data();
    info->version = versionText.data();
    return 1;
}

PLI_INT32 PliContext::control(PLI_INT32 operation) {
    switch (operation) {
    case vpiFinish:
        if (kernel != nullptr)
            kernel->requestFinish();
        else
            finishBeforeAttach = true;
        return 1;

    case vpiStop:
        if (kernel != nullptr) kernel->requestStop();
        return 1;

    default:
        fail(vpiError, std::format("vpi_control: operation {} is not supported", operation));
        return 0;
    }
}

std::ostream& PliContext::transcriptStream() const {
    return transcript != nullptr ? *transcript : std::cout;
}

PLI_INT32 PliContext::print(std::string_view text) {
    transcriptStream() << text;
    return static_cast<PLI_INT32>(text.size());
}

PLI_INT32 PliContext::flush() {
    transcriptStream().flush();
    return 0;
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

vpiHandle PliContext::registerCallback(const s_cb_data* data) {
    if (data == nullptr || data->cb_rtn == nullptr) {
        fail(vpiError, "vpi_register_cb: the callback data or its routine is a null pointer");
        return nullptr;
    }

    auto callback = std::make_unique<Callback>();
    callback->kind = Object::Kind::Callback;
    callback->type = vpiCallback;
    callback->data = *data;
    callback->data.time = nullptr;
    callback->data.value = nullptr;
    callback->timeType = data->time != nullptr ? data->time->type : vpiSuppressTime;
    callback->valueFormat = data->value != nullptr ? data->value->format : vpiSuppressVal;

    const runtime::SimTime now = kernel != nullptr ? kernel->time() : 0;
    Callback* raw = callback.get();

    switch (data->reason) {
    case cbValueChange: {
        const Object* subject = resolve(data->obj, "vpi_register_cb");
        if (subject == nullptr) return nullptr;
        if (subject->kind != Object::Kind::Signal || subject->isArray) {
            fail(vpiError, std::format("vpi_register_cb: a {} has no value to change",
                                       typeName(subject->type)));
            return nullptr;
        }
        callback->signal = subject->signal;
        callback->width = subject->width;
        callback->isSigned = subject->isSigned;
        callback->isWindow = subject->isWindow;
        callback->lowBit = subject->lowBit;
        if (subject->isWindow) callback->lastValue = readSignal(*subject);
        valueChangeWatchers[subject->signal].push_back(raw);
        break;
    }

    case cbAfterDelay:
    case cbAtStartOfSimTime: {
        const std::optional<runtime::SimTime> ticks =
            data->time != nullptr ? ticksOf(*data->time) : std::nullopt;
        if (!ticks.has_value()) {
            fail(vpiError, "vpi_register_cb: a timed callback needs a time Enzan can schedule");
            return nullptr;
        }
        const runtime::SimTime due = data->reason == cbAfterDelay ? now + *ticks : *ticks;
        if (due < now) {
            fail(vpiError, "vpi_register_cb: cbAtStartOfSimTime names a time already past");
            return nullptr;
        }
        timedCallbacks.emplace(due, raw);
        break;
    }

    // Registered during read-only, these belong to the next slot.
    case cbReadWriteSynch:
        (phase == Phase::ReadOnly ? deferredReadWrite : readWrite).push_back(raw);
        break;
    case cbReadOnlySynch:
        (phase == Phase::ReadOnly ? deferredReadOnly : readOnly).push_back(raw);
        break;

    case cbNextSimTime: nextSimTime.push_back(raw); break;
    case cbStartOfSimulation: startCallbacks.push_back(raw); break;
    case cbEndOfSimulation: endCallbacks.push_back(raw); break;

    default:
        fail(vpiError,
             std::format("vpi_register_cb: callback reason {} is not supported", data->reason));
        return nullptr;
    }

    objects.emplace(raw, std::move(callback));
    return reinterpret_cast<vpiHandle>(raw);
}

PLI_INT32 PliContext::removeCallback(vpiHandle handle) {
    Object* found = resolve(handle, "vpi_remove_cb");
    if (found == nullptr) return 0;
    auto* callback = dynamic_cast<Callback*>(found);
    if (callback == nullptr) {
        fail(vpiError, "vpi_remove_cb: the handle is not a callback");
        return 0;
    }

    retire(*callback);
    if (dispatchDepth == 0) sweepNow();
    return 1;
}

void PliContext::getCallbackInfo(vpiHandle handle, s_cb_data* data) {
    if (data == nullptr) {
        fail(vpiError, "vpi_get_cb_info: the callback data is a null pointer");
        return;
    }
    const Object* found = resolve(handle, "vpi_get_cb_info");
    if (found == nullptr) return;
    const auto* callback = dynamic_cast<const Callback*>(found);
    if (callback == nullptr) {
        fail(vpiError, "vpi_get_cb_info: the handle is not a callback");
        return;
    }
    *data = callback->data;
}

void PliContext::fire(Callback& callback) {
#ifdef ENZAN_WITH_VHDL
    if (callback.vhpiData.cb_rtn != nullptr) {
        vhpiFire(callback);
        return;
    }
#endif
    s_cb_data data = callback.data;

    s_vpi_time time{};
    time.type = callback.timeType;
    fillTime(time, kernel != nullptr ? kernel->time() : 0);
    data.time = &time;

    // Start and end of simulation fire with no design attached, so the kernel is checked here
    // as it is for the time above. A value-change callback never meets a null one — detaching
    // drops them all — but this function must not depend on which reason it was called for.
    s_vpi_value value{};
    value.format = callback.valueFormat;
    if (callback.data.reason == cbValueChange && value.format != vpiSuppressVal
        && kernel != nullptr) {
        std::string ignored;
        static_cast<void>(
            readValue(readSignal(callback).ref(), callback.isSigned, value, values, ignored));
    }
    data.value = &value;

    ++dispatchDepth;
    callback.data.cb_rtn(&data);
    --dispatchDepth;
}

void PliContext::retire(Callback& callback) {
    if (callback.removed) return;
    callback.removed = true;

    // Owned by the retired list until a sweep frees it: a dispatch further up the stack may be
    // holding this callback, and it must find it marked rather than freed.
    const auto found = objects.find(&callback);
    if (found != objects.end()) {
        retired.push_back(std::move(found->second));
        objects.erase(found);
    }
    sweepNeeded = true;
}

void PliContext::runOnce(std::vector<Callback*>& pending,
                         [[maybe_unused]] std::vector<Callback*>* rearm) {
    // Taken as a batch, so that a callback registering another of the same reason gets the
    // next occurrence rather than this one.
    std::vector<Callback*> batch;
    batch.swap(pending);

    ++dispatchDepth;
    for (Callback* callback : batch) {
        if (callback->removed) continue;
        fire(*callback);
#ifdef ENZAN_WITH_VHDL
        // A repetitive VHPI callback waits for the next occurrence, which `rearm` holds, unless
        // it removed itself as it ran.
        if (callback->repeats && rearm != nullptr && !callback->removed) {
            rearm->push_back(callback);
            continue;
        }
#endif
        retire(*callback);
    }
    --dispatchDepth;

    if (dispatchDepth == 0) sweepNow();
}

void PliContext::sweepNow() {
    if (!sweepNeeded) return;
    sweepNeeded = false;

    const auto isRemoved = [](const Callback* callback) { return callback->removed; };
    for (std::vector<Callback*>& watchers : valueChangeWatchers) std::erase_if(watchers, isRemoved);
    std::erase_if(timedCallbacks, [](const auto& entry) { return entry.second->removed; });
    std::erase_if(readWrite, isRemoved);
    std::erase_if(readOnly, isRemoved);
    std::erase_if(deferredReadWrite, isRemoved);
    std::erase_if(deferredReadOnly, isRemoved);
    std::erase_if(nextSimTime, isRemoved);
    std::erase_if(startCallbacks, isRemoved);
    std::erase_if(endCallbacks, isRemoved);

    retired.clear();
}

void PliContext::startOfSimulation() {
    if (started) return;
    started = true;
    runOnce(startCallbacks);
}

void PliContext::endOfSimulation() {
    if (ended) return;
    ended = true;
    runOnce(endCallbacks);
}

// ---------------------------------------------------------------------------
// runtime::PliHooks
// ---------------------------------------------------------------------------

void PliContext::valueChanged(std::uint32_t signalIndex) {
    if (signalIndex >= valueChangeWatchers.size()) return;
    if (valueChangeWatchers[signalIndex].empty()) return;

    // By index and up to the count on entry: a callback registered while this change is
    // delivered waits for the next one, and the list may grow while it is walked.
    ++dispatchDepth;
    const std::size_t count = valueChangeWatchers[signalIndex].size();
    for (std::size_t index = 0; index < count; ++index) {
        Callback* callback = valueChangeWatchers[signalIndex][index];
        if (callback->removed) continue;

        // A change of the signal is a change of a window only when the window moved.
        if (callback->isWindow) {
            eir::BitVector now = readSignal(*callback);
            if (now.ref().identicalTo(callback->lastValue.ref())) continue;
            callback->lastValue = std::move(now);
        }
        fire(*callback);
    }
    --dispatchDepth;

    if (dispatchDepth == 0) sweepNow();
}

runtime::SimTime PliContext::nextTimedEvent() const {
    runtime::SimTime next = runtime::NoTime;
    if (!delayedWrites.empty()) next = delayedWrites.begin()->first;
    for (const auto& [time, callback] : timedCallbacks) {
        if (callback->removed) continue;
        next = std::min(next, time);
        break;
    }
    return next;
}

void PliContext::timeAdvanced(runtime::SimTime /*time*/) {
    // What was registered during the last slot's read-only phase has waited for this.
    readWrite.insert(readWrite.end(), deferredReadWrite.begin(), deferredReadWrite.end());
    deferredReadWrite.clear();
    readOnly.insert(readOnly.end(), deferredReadOnly.begin(), deferredReadOnly.end());
    deferredReadOnly.clear();

    runOnce(nextSimTime, &nextSimTime);
}

void PliContext::runTimedEvents(runtime::SimTime time) {
    ++dispatchDepth;

    // Delayed writes first, so that a callback at the same time sees them.
    while (!delayedWrites.empty() && delayedWrites.begin()->first <= time) {
        const DelayedWrite write = std::move(delayedWrites.begin()->second);
        delayedWrites.erase(delayedWrites.begin());
        if (write.partial)
            kernel->writeSignalPart(write.signal, write.value.ref(), write.lowBit);
        else
            kernel->deposit(write.signal, write.value.ref());
    }

    std::vector<Callback*> due;
    while (!timedCallbacks.empty() && timedCallbacks.begin()->first <= time) {
        due.push_back(timedCallbacks.begin()->second);
        timedCallbacks.erase(timedCallbacks.begin());
    }
    runOnce(due);

    --dispatchDepth;
    if (dispatchDepth == 0) sweepNow();
}

void PliContext::runReadWrite() {
    phase = Phase::ReadWrite;
    runOnce(readWrite, &deferredReadWrite);
    phase = Phase::None;
}

void PliContext::runReadOnly() {
    phase = Phase::ReadOnly;
    runOnce(readOnly, &deferredReadOnly);
    phase = Phase::None;
}

} // namespace enzan::pli
