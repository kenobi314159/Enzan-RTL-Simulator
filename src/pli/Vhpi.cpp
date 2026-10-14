// SPDX-License-Identifier: GPL-3.0-or-later

// The VHDL Procedural Interface of PliContext (DESIGN_GUIDE.md section 9.7): the design's
// regions and objects as VHPI names them, their VHDL types from the definitions' tables, their
// values in VHPI's formats, and VHPI's callbacks on the same dispatch as VPI's.

#include "pli/PliContext.h"
#include "pli/PliModel.h"

#include "common/Version.h"
#include "eir/BitOps.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "elab/ElaboratedDesign.h"
#include "runtime/Image.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <utility>

namespace enzan::pli {

namespace {

using Kind = eir::VhdlType::Kind;

/// A basic identifier as VHPI's Name properties give it, in upper case (IEEE 1076 section
/// 19.4.3); Enzan keeps the lower, which its CaseName properties give, as NVC's do.
std::string upper(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char letter) {
        return static_cast<char>(std::toupper(letter));
    });
    return result;
}

/// The name a query gives, in the case Enzan keeps names in: lower, but within an extended
/// identifier, `\Name\`, as written.
std::string lowerOutsideExtended(std::string_view text) {
    std::string result(text);
    bool extended = false;
    for (char& character : result) {
        if (character == '\\') extended = !extended;
        if (!extended)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return result;
}

const char* className(std::int32_t kind) {
    switch (kind) {
    case vhpiRootInstK: return "vhpiRootInstK";
    case vhpiCompInstStmtK: return "vhpiCompInstStmtK";
    case vhpiBlockStmtK: return "vhpiBlockStmtK";
    case vhpiForGenerateK: return "vhpiForGenerateK";
    case vhpiSigDeclK: return "vhpiSigDeclK";
    case vhpiPortDeclK: return "vhpiPortDeclK";
    case vhpiGenericDeclK: return "vhpiGenericDeclK";
    case vhpiIndexedNameK: return "vhpiIndexedNameK";
    case vhpiSelectedNameK: return "vhpiSelectedNameK";
    case vhpiEnumTypeDeclK: return "vhpiEnumTypeDeclK";
    case vhpiIntTypeDeclK: return "vhpiIntTypeDeclK";
    case vhpiPhysTypeDeclK: return "vhpiPhysTypeDeclK";
    case vhpiFloatTypeDeclK: return "vhpiFloatTypeDeclK";
    case vhpiArrayTypeDeclK: return "vhpiArrayTypeDeclK";
    case vhpiRecordTypeDeclK: return "vhpiRecordTypeDeclK";
    case vhpiSubtypeDeclK: return "vhpiSubtypeDeclK";
    case vhpiIntRangeK: return "vhpiIntRangeK";
    case vhpiEnumLiteralK: return "vhpiEnumLiteralK";
    case vhpiToolK: return "vhpiToolK";
    case vhpiArgvK: return "vhpiArgvK";
    case vhpiIteratorK: return "vhpiIteratorK";
    case vhpiCallbackK: return "vhpiCallbackK";
    default: return "vhpiUndefined";
    }
}

/// True for a class that has a value: a declared signal, port or generic, or a part of one.
bool hasValue(std::int32_t kind) {
    return kind == vhpiSigDeclK || kind == vhpiPortDeclK || kind == vhpiGenericDeclK
           || kind == vhpiIndexedNameK || kind == vhpiSelectedNameK;
}

/// True for a region: an instance, a block, or one iteration of a generate statement.
bool isRegion(std::int32_t kind) {
    return kind == vhpiRootInstK || kind == vhpiCompInstStmtK || kind == vhpiBlockStmtK
           || kind == vhpiForGenerateK;
}

bool isTypeClass(std::int32_t kind) {
    return kind == vhpiEnumTypeDeclK || kind == vhpiIntTypeDeclK || kind == vhpiPhysTypeDeclK
           || kind == vhpiFloatTypeDeclK || kind == vhpiArrayTypeDeclK
           || kind == vhpiRecordTypeDeclK || kind == vhpiSubtypeDeclK;
}

std::int32_t typeClass(const eir::VhdlType& type) {
    switch (type.kind) {
    case Kind::Enumeration: return vhpiEnumTypeDeclK;
    case Kind::Integer: return vhpiIntTypeDeclK;
    case Kind::Physical: return vhpiPhysTypeDeclK;
    case Kind::Floating: return vhpiFloatTypeDeclK;
    case Kind::Array: return vhpiArrayTypeDeclK;
    case Kind::Record: return vhpiRecordTypeDeclK;
    }
    return vhpiUndefined;
}

/// How many elements an array has, over every dimension.
std::uint64_t elementCount(const eir::VhdlType& type) {
    std::uint64_t count = 1;
    for (const eir::VhdlType::Range& range : type.ranges) {
        const std::int64_t span =
            range.ascending ? range.right - range.left : range.left - range.right;
        count *= span < 0 ? 0 : static_cast<std::uint64_t>(span) + 1;
    }
    return count;
}

/// The character an enumeration literal is, `'U'` being U; none for an identifier.
std::optional<char> literalCharacter(std::string_view literal) {
    if (literal.size() == 3 && literal.front() == '\'' && literal.back() == '\'') return literal[1];
    return std::nullopt;
}

/// True for an enumeration whose literals are std_ulogic's, or bit's, which VHPI's Logic
/// formats carry.
bool isLogic(const eir::EnumDefinition& enumeration) {
    static constexpr std::string_view Std = "UX01ZWLH-";
    static constexpr std::string_view Bit = "01";
    std::string_view expected;
    if (enumeration.members.size() == Std.size()) expected = Std;
    if (enumeration.members.size() == Bit.size()) expected = Bit;
    if (expected.empty()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (literalCharacter(enumeration.members[index].name) != expected[index]) return false;
    }
    return true;
}

/// True for an enumeration of characters, CHARACTER or one like it, whose arrays are strings.
bool isCharacter(const eir::VhdlType& type, const eir::EnumDefinition& enumeration) {
    return type.name == "CHARACTER" || enumeration.members.size() == 256;
}

/// The unsigned number the low `width` bits from `lowBit` of `bits` hold.
std::uint64_t field(const eir::BitVector& bits, std::uint32_t lowBit, std::uint32_t width) {
    std::uint64_t value = 0;
    for (std::uint32_t bit = 0; bit < width && bit < 64; ++bit) {
        if (bits.ref().get(lowBit + bit) == eir::Logic::One) value |= std::uint64_t{1} << bit;
    }
    return value;
}

void setField(eir::BitVector& bits, std::uint32_t lowBit, std::uint32_t width,
              std::uint64_t value) {
    for (std::uint32_t bit = 0; bit < width; ++bit) {
        const bool one = bit < 64 && ((value >> bit) & 1U) != 0;
        bits.mutableRef().set(lowBit + bit, one ? eir::Logic::One : eir::Logic::Zero);
    }
}

std::int64_t signExtend(std::uint64_t value, std::uint32_t width) {
    if (width == 0 || width >= 64) return static_cast<std::int64_t>(value);
    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

} // namespace

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

PliContext::Object* PliContext::vhpiResolve(vhpiHandleT handle, std::string_view routine) {
    const auto found = objects.find(static_cast<const void*>(handle));
    if (handle == nullptr || found == objects.end()
        || (found->second->kind != Object::Kind::Vhpi
            && found->second->kind != Object::Kind::Callback)) {
        fail(vpiError,
             std::format("{}: the handle is not one Enzan gave, or has been released", routine));
        return nullptr;
    }
    return found->second.get();
}

vhpiHandleT PliContext::vhpiKeep(std::unique_ptr<Object> object) {
    if (object == nullptr) return nullptr;
    Object* raw = object.get();
    objects.emplace(raw, std::move(object));
    return reinterpret_cast<vhpiHandleT>(raw);
}

const eir::ModuleDefinition& PliContext::definitionOf(std::uint32_t instanceIndex) const {
    return image->analysedDesign().module(
        image->elaboratedDesign().instances()[instanceIndex].definitionIndex);
}

const eir::VhdlType* PliContext::vhdlTypeOf(const Object& object) const {
    if (object.vhdlType == eir::VhdlType::None) return nullptr;
    const eir::ModuleDefinition& definition = definitionOf(object.instance);
    if (object.vhdlType >= definition.vhdlTypes.size()) return nullptr;
    return &definition.vhdlTypes[object.vhdlType];
}

namespace {

/// The VHPI path of a region, `:top:u:g(0)`, from the scopes above it.
std::string regionPath(const std::vector<std::string>& names) {
    std::string path;
    for (const std::string& name : names) path += ':' + name;
    return path;
}

} // namespace

std::unique_ptr<PliContext::Object> PliContext::vhpiRegion(std::uint32_t scopeIndex) const {
    const Scope& scope = scopes[scopeIndex];
    auto region = std::make_unique<Object>();
    region->kind = Object::Kind::Vhpi;
    region->scope = scopeIndex;
    region->instance = scope.instance;
    if (scope.isModule) {
        region->vhpiClass = scope.parent == NoScope ? vhpiRootInstK : vhpiCompInstStmtK;
    } else {
        region->vhpiClass = scope.name.ends_with(')') ? vhpiForGenerateK : vhpiBlockStmtK;
    }

    // A root instance is named for the entity it is (IEEE 1076 section 19.4.3).
    std::vector<std::string> names;
    for (std::uint32_t at = scopeIndex; at != NoScope; at = scopes[at].parent) {
        names.push_back(scopes[at].parent == NoScope ? definitionOf(scopes[at].instance).sourceName
                                                     : scopes[at].name);
    }
    std::ranges::reverse(names);
    region->name = names.back();
    region->fullName = regionPath(names);
    return region;
}

std::unique_ptr<PliContext::Object> PliContext::vhpiDeclared(std::uint32_t instanceIndex,
                                                             std::uint32_t objectIndex,
                                                             bool isGeneric) const {
    const eir::ModuleDefinition& definition = definitionOf(instanceIndex);
    auto declared = std::make_unique<Object>();
    declared->kind = Object::Kind::Vhpi;
    declared->instance = instanceIndex;

    std::string localName;
    if (isGeneric) {
        const eir::Parameter& parameter = definition.parameters[objectIndex];
        declared->vhpiClass = vhpiGenericDeclK;
        declared->vhdlType = parameter.vhdlType;
        declared->target =
            Target{.kind = TargetKind::Parameter, .first = instanceIndex, .second = objectIndex};
        declared->scope = parameterScopes[instanceIndex][objectIndex];
        declared->width = parameter.value.width();
        localName = parameter.name;
    } else {
        const eir::Object& object = definition.objects[objectIndex];
        const bool isPort = std::ranges::any_of(definition.ports, [&](const eir::Port& port) {
            return port.objectIndex == objectIndex;
        });
        declared->vhpiClass = isPort ? vhpiPortDeclK : vhpiSigDeclK;
        declared->vhdlType = object.vhdlType;
        declared->target =
            Target{.kind = TargetKind::Signal, .first = instanceIndex, .second = objectIndex};
        declared->signal =
            image->elaboratedDesign().instances()[instanceIndex].objectSlots[objectIndex];
        declared->width = object.type.bitWidth();
        // The scope of the generate block or block it is in: found as buildScopes found it.
        declared->scope = moduleScopeOfInstance[instanceIndex];
        for (std::size_t depth = 0;; ++depth) {
            bool deeper = false;
            for (const std::uint32_t child : scopes[declared->scope].children) {
                if (scopes[child].isModule || scopes[child].instance != instanceIndex) continue;
                if (object.name.starts_with(scopes[child].prefix)) {
                    declared->scope = child;
                    deeper = true;
                    break;
                }
            }
            if (!deeper || depth > object.name.size()) break;
        }
        localName = object.name;
    }
    localName = localName.substr(scopes[declared->scope].prefix.size());
    declared->name = localName;
    declared->fullName = vhpiRegion(declared->scope)->fullName + ':' + localName;
    return declared;
}

std::unique_ptr<PliContext::Object> PliContext::vhpiElement(const Object& parent,
                                                            std::uint64_t flatIndex) const {
    const eir::VhdlType* type = vhdlTypeOf(parent);
    if (type == nullptr || type->kind != Kind::Array) return nullptr;
    const std::uint64_t count = elementCount(*type);
    if (flatIndex >= count) return nullptr;
    const eir::VhdlType& element = definitionOf(parent.instance).vhdlTypes[type->element];

    // Row by row from the left, which is the highest bits (section 3.6).
    auto indexed = std::make_unique<Object>();
    indexed->kind = Object::Kind::Vhpi;
    indexed->vhpiClass = vhpiIndexedNameK;
    indexed->instance = parent.instance;
    indexed->vhdlType = type->element;
    indexed->target = parent.target;
    indexed->scope = parent.scope;
    indexed->signal = parent.signal;
    indexed->width = element.width;
    indexed->isWindow = true;
    indexed->lowBit =
        parent.lowBit + static_cast<std::uint32_t>((count - 1 - flatIndex) * element.width);

    std::string indices;
    std::uint64_t rest = flatIndex;
    std::uint64_t below = count;
    for (const eir::VhdlType::Range& range : type->ranges) {
        const std::int64_t span =
            range.ascending ? range.right - range.left : range.left - range.right;
        const std::uint64_t length = static_cast<std::uint64_t>(span) + 1;
        below /= length;
        const auto position = static_cast<std::int64_t>(rest / below);
        rest %= below;
        if (!indices.empty()) indices += ',';
        indices += std::to_string(range.ascending ? range.left + position : range.left - position);
    }
    indexed->name = parent.name + '(' + indices + ')';
    indexed->fullName = parent.fullName + '(' + indices + ')';
    return indexed;
}

std::unique_ptr<PliContext::Object> PliContext::vhpiField(const Object& parent,
                                                          std::size_t fieldIndex) const {
    const eir::VhdlType* type = vhdlTypeOf(parent);
    if (type == nullptr || type->kind != Kind::Record || fieldIndex >= type->fields.size())
        return nullptr;
    const eir::VhdlType::Field& recordField = type->fields[fieldIndex];
    auto selected = std::make_unique<Object>();
    selected->kind = Object::Kind::Vhpi;
    selected->vhpiClass = vhpiSelectedNameK;
    selected->instance = parent.instance;
    selected->vhdlType = recordField.type;
    selected->target = parent.target;
    selected->scope = parent.scope;
    selected->signal = parent.signal;
    selected->width = definitionOf(parent.instance).vhdlTypes[recordField.type].width;
    selected->isWindow = true;
    selected->lowBit = parent.lowBit + recordField.offset;
    const std::string fieldName = lowerOutsideExtended(recordField.name);
    selected->name = parent.name + '.' + fieldName;
    selected->fullName = parent.fullName + '.' + fieldName;
    return selected;
}

std::unique_ptr<PliContext::Object> PliContext::vhpiTypeHandle(const Object& of, std::uint32_t type,
                                                               bool base) const {
    const eir::ModuleDefinition& definition = definitionOf(of.instance);
    if (type >= definition.vhdlTypes.size()) return nullptr;
    auto described = std::make_unique<Object>();
    described->kind = Object::Kind::Vhpi;
    described->instance = of.instance;
    described->vhdlType = type;
    described->baseType = base;
    described->vhpiClass = base ? typeClass(definition.vhdlTypes[type]) : vhpiSubtypeDeclK;
    described->name = definition.vhdlTypes[type].name;
    described->fullName = described->name;
    return described;
}

std::vector<std::unique_ptr<PliContext::Object>> PliContext::vhpiChildren(vhpiOneToManyT type,
                                                                          const Object& reference) {
    std::vector<std::unique_ptr<Object>> children;
    if (isRegion(reference.vhpiClass)) {
        const Scope& scope = scopes[reference.scope];
        const eir::ModuleDefinition& definition = definitionOf(scope.instance);
        switch (type) {
        case vhpiInternalRegions:
            for (const std::uint32_t child : scope.children) {
                if (!scopes[child].isPackage) children.push_back(vhpiRegion(child));
            }
            break;
        case vhpiSigDecls:
        case vhpiPortDecls:
            for (const std::uint32_t objectIndex : scope.vhdlObjects) {
                std::unique_ptr<Object> declared = vhpiDeclared(scope.instance, objectIndex, false);
                const bool wanted = type == vhpiPortDecls ? declared->vhpiClass == vhpiPortDeclK
                                                          : declared->vhpiClass == vhpiSigDeclK;
                if (wanted) children.push_back(std::move(declared));
            }
            break;
        case vhpiGenericDecls:
            for (const std::uint32_t parameterIndex : scope.parameters) {
                if (definition.parameters[parameterIndex].vhdlType != eir::VhdlType::None)
                    children.push_back(vhpiDeclared(scope.instance, parameterIndex, true));
            }
            break;
        default: break;
        }
        return children;
    }

    if (hasValue(reference.vhpiClass)) {
        const eir::VhdlType* described = vhdlTypeOf(reference);
        if (described == nullptr) return children;
        if (type == vhpiIndexedNames && described->kind == Kind::Array) {
            const std::uint64_t count = elementCount(*described);
            for (std::uint64_t index = 0; index < count; ++index)
                children.push_back(vhpiElement(reference, index));
        }
        if (type == vhpiSelectedNames && described->kind == Kind::Record) {
            for (std::size_t index = 0; index < described->fields.size(); ++index)
                children.push_back(vhpiField(reference, index));
        }
        return children;
    }

    if (isTypeClass(reference.vhpiClass)) {
        const eir::ModuleDefinition& definition = definitionOf(reference.instance);
        const eir::VhdlType& described = definition.vhdlTypes[reference.vhdlType];
        if (type == vhpiConstraints) {
            // A base type of an array is held unconstrained: the bounds it has are those of the
            // object's subtype, which an application asks for when it finds none here.
            for (const eir::VhdlType::Range& range : described.ranges) {
                auto bounds = std::make_unique<Object>();
                bounds->kind = Object::Kind::Vhpi;
                bounds->vhpiClass = vhpiIntRangeK;
                bounds->left = range.left;
                bounds->right = range.right;
                bounds->number = range.ascending ? 1 : 0;
                bounds->baseType = reference.baseType && described.kind == Kind::Array;
                children.push_back(std::move(bounds));
            }
        }
        if (type == vhpiEnumLiterals && described.kind == Kind::Enumeration) {
            const eir::EnumDefinition& enumeration = definition.enums[described.enumIndex];
            for (std::size_t position = 0; position < enumeration.members.size(); ++position) {
                auto literal = std::make_unique<Object>();
                literal->kind = Object::Kind::Vhpi;
                literal->vhpiClass = vhpiEnumLiteralK;
                literal->number = static_cast<std::int64_t>(position);
                // A character literal keeps its case, 'a' being another value than 'A'.
                const std::string& member = enumeration.members[position].name;
                literal->text = member.starts_with('\'') ? member : upper(member);
                literal->name = literal->text;
                children.push_back(std::move(literal));
            }
        }
        return children;
    }

    if (reference.vhpiClass == vhpiToolK && type == vhpiArgvs) {
        for (const std::string& argument : arguments) {
            auto argv = std::make_unique<Object>();
            argv->kind = Object::Kind::Vhpi;
            argv->vhpiClass = vhpiArgvK;
            argv->text = argument;
            argv->name = argument;
            children.push_back(std::move(argv));
        }
    }
    return children;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

vhpiHandleT PliContext::vhpiHandle(vhpiOneToOneT type, vhpiHandleT reference) {
    clearError();
    if (reference == nullptr) {
        if (type == vhpiTool) {
            auto tool = std::make_unique<Object>();
            tool->kind = Object::Kind::Vhpi;
            tool->vhpiClass = vhpiToolK;
            tool->name = product;
            tool->text = versionText;
            return vhpiKeep(std::move(tool));
        }
        if (type == vhpiRootInst && image != nullptr) {
            for (std::uint32_t index = 0; index < scopes.size(); ++index) {
                if (scopes[index].isModule && scopes[index].parent == NoScope
                    && !scopes[index].isPackage)
                    return vhpiKeep(vhpiRegion(index));
            }
        }
        return nullptr;
    }

    const Object* found = vhpiResolve(reference, "vhpi_handle");
    if (found == nullptr) return nullptr;
    switch (type) {
    case vhpiBaseType:
        if (hasValue(found->vhpiClass) || isTypeClass(found->vhpiClass))
            return vhpiKeep(vhpiTypeHandle(*found, found->vhdlType, true));
        return nullptr;
    // vhpiSubtype and vhpiElemSubtype, which IEEE 1076-2019 deprecates and its header renames,
    // are still what an application written against an older one asks for.
    case vhpiType:
    case DEPRECATED_vhpiSubtype:
        if (hasValue(found->vhpiClass))
            return vhpiKeep(vhpiTypeHandle(*found, found->vhdlType, false));
        return nullptr;
    case vhpiElemType:
    case DEPRECATED_vhpiElemSubtype: {
        if (!isTypeClass(found->vhpiClass)) return nullptr;
        const eir::VhdlType* described = vhdlTypeOf(*found);
        if (described == nullptr || described->kind != Kind::Array) return nullptr;
        return vhpiKeep(vhpiTypeHandle(*found, described->element, type == vhpiElemType));
    }
    default: return nullptr;
    }
}

vhpiHandleT PliContext::vhpiIterator(vhpiOneToManyT type, vhpiHandleT reference) {
    clearError();
    std::vector<std::unique_ptr<Object>> children;
    if (reference == nullptr) {
        // Below nothing is the root instance, where an application starting from it begins.
        if (type == vhpiInternalRegions || type == vhpiCompInstStmts) {
            vhpiHandleT root = vhpiHandle(vhpiRootInst, nullptr);
            if (root != nullptr) {
                const auto owned = objects.find(static_cast<const void*>(root));
                children.push_back(std::move(owned->second));
                objects.erase(owned);
            }
        }
    } else {
        const Object* found = vhpiResolve(reference, "vhpi_iterator");
        if (found == nullptr) return nullptr;
        children = vhpiChildren(type, *found);
    }
    std::erase(children, nullptr);
    if (children.empty()) return nullptr;

    auto iterator = std::make_unique<Object>();
    iterator->kind = Object::Kind::Vhpi;
    iterator->vhpiClass = vhpiIteratorK;
    iterator->iteratorType = type;
    iterator->pending = std::move(children);
    return vhpiKeep(std::move(iterator));
}

vhpiHandleT PliContext::vhpiScan(vhpiHandleT iterator) {
    clearError();
    Object* found = vhpiResolve(iterator, "vhpi_scan");
    if (found == nullptr) return nullptr;
    if (found->vhpiClass != vhpiIteratorK) {
        fail(vpiError, "vhpi_scan: the handle is not an iterator");
        return nullptr;
    }
    // The last scan frees the iterator, as the standard says an exhausted one is.
    if (found->position >= found->pending.size()) {
        objects.erase(static_cast<const void*>(iterator));
        return nullptr;
    }
    return vhpiKeep(std::move(found->pending[found->position++]));
}

vhpiHandleT PliContext::vhpiHandleByIndex(vhpiOneToManyT type, vhpiHandleT parent,
                                          std::int32_t index) {
    clearError();
    const Object* found = vhpiResolve(parent, "vhpi_handle_by_index");
    if (found == nullptr || index < 0) return nullptr;
    if (type == vhpiIndexedNames && hasValue(found->vhpiClass))
        return vhpiKeep(vhpiElement(*found, static_cast<std::uint64_t>(index)));
    if (type == vhpiSelectedNames && hasValue(found->vhpiClass))
        return vhpiKeep(vhpiField(*found, static_cast<std::size_t>(index)));
    std::vector<std::unique_ptr<Object>> children = vhpiChildren(type, *found);
    if (static_cast<std::size_t>(index) >= children.size()) return nullptr;
    return vhpiKeep(std::move(children[static_cast<std::size_t>(index)]));
}

vhpiHandleT PliContext::vhpiHandleByName(const char* name, vhpiHandleT scope) {
    clearError();
    if (name == nullptr || image == nullptr) return nullptr;

    // A path from the root, `:top:u:sig` or `top.u.sig`, or from `scope`; selections after the
    // object's name, `sig(3)` and `rec.field`, are taken one by one below.
    std::string path = lowerOutsideExtended(name);
    if (path.starts_with(':')) path.erase(0, 1);
    std::ranges::replace(path, ':', '.');

    std::vector<std::string> parts;
    std::string part;
    unsigned depth = 0;
    for (const char character : path) {
        if (character == '(') ++depth;
        if (character == ')' && depth > 0) --depth;
        if (character == '.' && depth == 0) {
            parts.push_back(part);
            part.clear();
            continue;
        }
        part.push_back(character);
    }
    parts.push_back(part);

    std::unique_ptr<Object> current;
    std::size_t next = 0;
    if (scope != nullptr) {
        const Object* found = vhpiResolve(scope, "vhpi_handle_by_name");
        if (found == nullptr || !isRegion(found->vhpiClass)) return nullptr;
        current = vhpiRegion(found->scope);
    } else {
        vhpiHandleT root = vhpiHandle(vhpiRootInst, nullptr);
        if (root == nullptr) return nullptr;
        const auto owned = objects.find(static_cast<const void*>(root));
        current = std::move(owned->second);
        objects.erase(owned);
        if (parts.empty() || parts.front() != lowerOutsideExtended(current->name)) return nullptr;
        next = 1;
    }

    for (; next < parts.size() && current != nullptr; ++next) {
        const std::string& wanted = parts[next];
        std::unique_ptr<Object> found;
        if (isRegion(current->vhpiClass)) {
            for (const vhpiOneToManyT relation :
                 {vhpiInternalRegions, vhpiPortDecls, vhpiSigDecls, vhpiGenericDecls}) {
                for (std::unique_ptr<Object>& child : vhpiChildren(relation, *current)) {
                    if (child != nullptr && child->name == wanted) found = std::move(child);
                    if (found != nullptr) break;
                }
                if (found != nullptr) break;
            }
            // An element of an object, `sig(3)`: the object, then the element.
            if (found == nullptr && wanted.ends_with(')')) {
                const std::size_t open = wanted.find('(');
                const std::string base = wanted.substr(0, open);
                for (const vhpiOneToManyT relation : {vhpiPortDecls, vhpiSigDecls}) {
                    for (std::unique_ptr<Object>& child : vhpiChildren(relation, *current)) {
                        if (child == nullptr || child->name != base) continue;
                        for (std::unique_ptr<Object>& element :
                             vhpiChildren(vhpiIndexedNames, *child)) {
                            if (element != nullptr && element->name == wanted)
                                found = std::move(element);
                            if (found != nullptr) break;
                        }
                    }
                }
            }
        } else if (hasValue(current->vhpiClass)) {
            const std::string selected = current->name + '.' + wanted;
            for (std::unique_ptr<Object>& child : vhpiChildren(vhpiSelectedNames, *current)) {
                if (child != nullptr && child->name == selected) found = std::move(child);
                if (found != nullptr) break;
            }
        }
        current = std::move(found);
    }
    return vhpiKeep(std::move(current));
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

vhpiIntT PliContext::vhpiGet(vhpiIntPropertyT property, vhpiHandleT object) {
    clearError();
    const Object* found = vhpiResolve(object, "vhpi_get");
    if (found == nullptr) return vhpiUndefined;
    const eir::VhdlType* described = vhdlTypeOf(*found);

    switch (property) {
    case vhpiKindP:
        if (found->kind == Object::Kind::Callback) return vhpiCallbackK;
        return found->vhpiClass;
    case vhpiSizeP:
        // The number of scalar subelements, which a string's length is.
        if (described == nullptr) return vhpiUndefined;
        if (described->kind == Kind::Array) return static_cast<vhpiIntT>(elementCount(*described));
        return 1;
    case vhpiNumDimensionsP:
        return described != nullptr && described->kind == Kind::Array
                   ? static_cast<vhpiIntT>(described->ranges.size())
                   : 0;
    case vhpiNumLiteralsP:
        if (described == nullptr || described->kind != Kind::Enumeration) return vhpiUndefined;
        return static_cast<vhpiIntT>(
            definitionOf(found->instance).enums[described->enumIndex].members.size());
    case vhpiLeftBoundP: return static_cast<vhpiIntT>(found->left);
    case vhpiRightBoundP: return static_cast<vhpiIntT>(found->right);
    case vhpiIsUpP: return static_cast<vhpiIntT>(found->number);
    case vhpiIsUnconstrainedP: return found->baseType ? 1 : 0;
    case vhpiPositionP: return static_cast<vhpiIntT>(found->number);
    case vhpiStaticnessP:
        return found->vhpiClass == vhpiGenericDeclK ? vhpiGloballyStatic : vhpiDynamic;
    case vhpiArgcP: return static_cast<vhpiIntT>(arguments.size());
    default: return vhpiUndefined;
    }
}

const vhpiCharT* PliContext::vhpiGetStr(vhpiStrPropertyT property, vhpiHandleT object) {
    clearError();
    const Object* found = vhpiResolve(object, "vhpi_get_str");
    if (found == nullptr) return nullptr;
    switch (property) {
    // A VHDL name is a basic identifier, given in upper case; a tool's and an argument's are not.
    // Neither NVC's analysis nor Enzan keeps the case a name was declared in, so its CaseName is
    // its Name, as NVC gives it (section 9.7).
    case vhpiNameP:
    case vhpiCaseNameP:
        vhpiString = found->vhpiClass == vhpiToolK || found->vhpiClass == vhpiArgvK
                         ? found->name
                         : upper(found->name);
        break;
    case vhpiFullNameP:
    case vhpiFullCaseNameP: vhpiString = upper(found->fullName); break;
    case vhpiKindStrP:
        vhpiString =
            className(found->kind == Object::Kind::Callback ? vhpiCallbackK : found->vhpiClass);
        break;
    case vhpiStrValP:
    case vhpiToolVersionP: vhpiString = found->text; break;
    case vhpiFileNameP: vhpiString = found->file; break;
    default: return nullptr;
    }
    return reinterpret_cast<const vhpiCharT*>(vhpiString.c_str());
}

vhpiPhysT PliContext::vhpiGetPhys(vhpiPhysPropertyT property, vhpiHandleT /*object*/) {
    clearError();
    vhpiPhysT result{};
    if (property != vhpiResolutionLimitP) {
        fail(vpiError, std::format("vhpi_get_phys: property {} is not supported",
                                   static_cast<int>(property)));
        return result;
    }
    // Femtoseconds in one of the kernel's units, which every time is counted in (section 3.1).
    const int exponent = kernel != nullptr ? kernel->timeScale().precisionExponent() : -15;
    std::uint64_t femtoseconds = 1;
    for (int power = -15; power < exponent; ++power) femtoseconds *= 10;
    result.high = static_cast<int32_t>(femtoseconds >> 32U);
    result.low = static_cast<uint32_t>(femtoseconds);
    return result;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

eir::BitVector PliContext::vhpiBits(const Object& object) const {
    if (object.target.kind == TargetKind::Parameter) {
        const eir::Parameter& parameter =
            definitionOf(object.target.first)
                .parameters[static_cast<std::size_t>(object.target.second)];
        eir::BitVector window(object.width);
        eir::bitops::partSelectAt(window.mutableRef(), parameter.value.ref(), object.lowBit);
        return window;
    }
    return readSignal(object);
}

int PliContext::vhpiFormat(const Object& object, const eir::BitVector& bits, vhpiValueT& value) {
    const eir::VhdlType* described = vhdlTypeOf(object);
    if (described == nullptr) {
        fail(vpiError, std::format("vhpi_get_value: '{}' has no VHDL type", object.fullName));
        return -1;
    }
    const eir::ModuleDefinition& definition = definitionOf(object.instance);
    const bool array = described->kind == Kind::Array;
    const eir::VhdlType& scalar = array ? definition.vhdlTypes[described->element] : *described;
    const eir::EnumDefinition* enumeration =
        scalar.kind == Kind::Enumeration ? &definition.enums[scalar.enumIndex] : nullptr;
    const bool logic = enumeration != nullptr && isLogic(*enumeration);
    const bool characters = enumeration != nullptr && isCharacter(scalar, *enumeration);

    // The format the object's type has, when the application asks for it.
    if (value.format == vhpiObjTypeVal) {
        if (described->kind == Kind::Record) {
            fail(vpiError,
                 std::format("vhpi_get_value: '{}' is a record; read its fields", object.fullName));
            return -1;
        }
        if (!array) {
            switch (scalar.kind) {
            case Kind::Enumeration: value.format = logic ? vhpiLogicVal : vhpiEnumVal; break;
            case Kind::Integer:
                value.format = scalar.width > 32 ? vhpiLongIntVal : vhpiIntVal;
                break;
            case Kind::Floating: value.format = vhpiRealVal; break;
            case Kind::Physical: value.format = vhpiPhysVal; break;
            default: break;
            }
        } else if (characters) {
            value.format = vhpiStrVal;
        } else if (enumeration != nullptr) {
            value.format = logic ? vhpiLogicVecVal : vhpiEnumVecVal;
        } else if (scalar.kind == Kind::Integer) {
            value.format = vhpiIntVecVal;
        } else if (scalar.kind == Kind::Floating) {
            value.format = vhpiRealVecVal;
        } else {
            value.format = vhpiBinStrVal;
        }
    }

    const std::uint64_t count = array ? elementCount(*described) : 1;
    const std::uint32_t elementWidth = scalar.width;
    // The element at `position` from the left, as the number its bits hold.
    const auto element = [&](std::uint64_t position) {
        return field(bits, static_cast<std::uint32_t>((count - 1 - position) * elementWidth),
                     elementWidth);
    };
    const auto literalText = [&](std::uint64_t code) -> char {
        if (enumeration == nullptr || code >= enumeration->members.size()) return '?';
        return literalCharacter(enumeration->members[code].name).value_or('?');
    };
    const auto room = [&](std::size_t bytes) -> int {
        if (value.bufSize >= bytes) return 0;
        return static_cast<int>(bytes);
    };

    switch (value.format) {
    case vhpiEnumVal:
    case vhpiLogicVal: value.value.enumv = static_cast<vhpiEnumT>(element(0)); return 0;
    case vhpiSmallEnumVal:
        value.value.smallenumv = static_cast<vhpiSmallEnumT>(element(0));
        return 0;
    case vhpiCharVal: value.value.ch = static_cast<vhpiCharT>(element(0)); return 0;
    case vhpiIntVal:
        value.value.intg = static_cast<vhpiIntT>(signExtend(element(0), elementWidth));
        return 0;
    case vhpiLongIntVal: value.value.longintg = signExtend(element(0), elementWidth); return 0;
    case vhpiRealVal: value.value.real = std::bit_cast<double>(element(0)); return 0;
    case vhpiPhysVal: {
        const auto amount = static_cast<std::uint64_t>(signExtend(element(0), elementWidth));
        value.value.phys.high = static_cast<int32_t>(amount >> 32U);
        value.value.phys.low = static_cast<uint32_t>(amount);
        return 0;
    }
    case vhpiTimeVal: {
        const auto amount = static_cast<std::uint64_t>(signExtend(element(0), elementWidth));
        value.value.time.high = static_cast<int32_t>(amount >> 32U);
        value.value.time.low = static_cast<uint32_t>(amount);
        return 0;
    }
    case vhpiEnumVecVal:
    case vhpiLogicVecVal:
    case vhpiSmallEnumVecVal:
    case vhpiIntVecVal:
    case vhpiRealVecVal: {
        std::size_t size = sizeof(vhpiEnumT);
        if (value.format == vhpiSmallEnumVecVal) size = sizeof(vhpiSmallEnumT);
        if (value.format == vhpiIntVecVal) size = sizeof(vhpiIntT);
        if (value.format == vhpiRealVecVal) size = sizeof(vhpiRealT);
        value.numElems = static_cast<int32_t>(count);
        if (const int needed = room(count * size); needed != 0) return needed;
        for (std::uint64_t position = 0; position < count; ++position) {
            const std::uint64_t code = element(position);
            switch (value.format) {
            case vhpiSmallEnumVecVal:
                value.value.smallenumvs[position] = static_cast<vhpiSmallEnumT>(code);
                break;
            case vhpiIntVecVal:
                value.value.intgs[position] = static_cast<vhpiIntT>(signExtend(code, elementWidth));
                break;
            case vhpiRealVecVal: value.value.reals[position] = std::bit_cast<double>(code); break;
            default: value.value.enumvs[position] = static_cast<vhpiEnumT>(code); break;
            }
        }
        return 0;
    }
    case vhpiStrVal: {
        value.numElems = static_cast<int32_t>(count);
        if (const int needed = room(count + 1); needed != 0) return needed;
        for (std::uint64_t position = 0; position < count; ++position)
            value.value.str[position] = static_cast<vhpiCharT>(element(position));
        value.value.str[count] = '\0';
        return 0;
    }
    case vhpiBinStrVal: {
        // A logic value is its literals' characters; anything else its bits, the highest first.
        const bool literals = logic;
        const std::uint64_t length = literals ? count : count * elementWidth;
        value.numElems = static_cast<int32_t>(length);
        if (const int needed = room(length + 1); needed != 0) return needed;
        for (std::uint64_t position = 0; position < length; ++position) {
            char digit = '0';
            if (literals) {
                digit = literalText(element(position));
            } else if (bits.ref().get(static_cast<std::uint32_t>(length - 1 - position))
                       == eir::Logic::One) {
                digit = '1';
            }
            value.value.str[position] = static_cast<vhpiCharT>(digit);
        }
        value.value.str[length] = '\0';
        return 0;
    }
    default:
        fail(vpiError, std::format("vhpi_get_value: format {} is not supported",
                                   static_cast<int>(value.format)));
        return -1;
    }
}

int PliContext::vhpiGetValue(vhpiHandleT object, vhpiValueT* value) {
    clearError();
    const Object* found = vhpiResolve(object, "vhpi_get_value");
    if (found == nullptr) return -1;
    if (value == nullptr || !hasValue(found->vhpiClass)) {
        fail(vpiError, "vhpi_get_value: the handle has no value, or the value is a null pointer");
        return -1;
    }
    return vhpiFormat(*found, vhpiBits(*found), *value);
}

int PliContext::vhpiPutValue(vhpiHandleT object, vhpiValueT* value, vhpiPutValueModeT mode) {
    clearError();
    const Object* found = vhpiResolve(object, "vhpi_put_value");
    if (found == nullptr) return -1;
    if (!hasValue(found->vhpiClass) || found->vhpiClass == vhpiGenericDeclK) {
        fail(vpiError, std::format("vhpi_put_value: '{}' cannot be written", found->fullName));
        return -1;
    }
    if (phase == Phase::ReadOnly) {
        fail(vpiError, "vhpi_put_value: nothing may be written at the end of a time step");
        return -1;
    }
    if (mode == vhpiRelease) return vhpiWrite(*found, eir::BitVector(found->width), mode) ? 0 : -1;
    if (value == nullptr) {
        fail(vpiError, "vhpi_put_value: the value is a null pointer");
        return -1;
    }

    const eir::VhdlType* described = vhdlTypeOf(*found);
    if (described == nullptr || described->kind == Kind::Record) {
        fail(vpiError,
             std::format("vhpi_put_value: '{}' is written field by field", found->fullName));
        return -1;
    }
    const eir::ModuleDefinition& definition = definitionOf(found->instance);
    const bool array = described->kind == Kind::Array;
    const eir::VhdlType& scalar = array ? definition.vhdlTypes[described->element] : *described;
    const std::uint64_t count = array ? elementCount(*described) : 1;
    const std::uint32_t elementWidth = scalar.width;
    eir::BitVector bits(found->width);
    std::ranges::fill(bits.mutableRef().avalWords(), 0);
    std::ranges::fill(bits.mutableRef().bvalWords(), 0);
    // An enumeration takes only its literals' positions, which the design relies on: an index of
    // one is not checked against the bounds of the array it indexes.
    const std::uint64_t literals =
        scalar.kind == Kind::Enumeration ? definition.enums[scalar.enumIndex].members.size() : 0;
    bool valid = true;
    const auto put = [&](std::uint64_t position, std::uint64_t code) {
        if (scalar.kind == Kind::Enumeration && code >= literals) valid = false;
        setField(bits, static_cast<std::uint32_t>((count - 1 - position) * elementWidth),
                 elementWidth, code);
    };

    switch (value->format) {
    case vhpiEnumVal:
    case vhpiLogicVal: put(0, value->value.enumv); break;
    case vhpiSmallEnumVal: put(0, value->value.smallenumv); break;
    case vhpiCharVal: put(0, static_cast<unsigned char>(value->value.ch)); break;
    case vhpiIntVal:
        put(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(value->value.intg)));
        break;
    case vhpiLongIntVal: put(0, static_cast<std::uint64_t>(value->value.longintg)); break;
    case vhpiRealVal: put(0, std::bit_cast<std::uint64_t>(value->value.real)); break;
    case vhpiEnumVecVal:
    case vhpiLogicVecVal:
    case vhpiSmallEnumVecVal:
    case vhpiIntVecVal:
    case vhpiRealVecVal:
    case vhpiStrVal: {
        if (std::cmp_not_equal(value->numElems, count)) {
            fail(vpiError, std::format("vhpi_put_value: '{}' has {} elements, and the value {}",
                                       found->fullName, count, value->numElems));
            return -1;
        }
        for (std::uint64_t position = 0; position < count; ++position) {
            std::uint64_t code = 0;
            switch (value->format) {
            case vhpiSmallEnumVecVal: code = value->value.smallenumvs[position]; break;
            case vhpiIntVecVal:
                code = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(value->value.intgs[position]));
                break;
            case vhpiRealVecVal:
                code = std::bit_cast<std::uint64_t>(value->value.reals[position]);
                break;
            case vhpiStrVal: code = static_cast<unsigned char>(value->value.str[position]); break;
            default: code = value->value.enumvs[position]; break;
            }
            put(position, code);
        }
        break;
    }
    default:
        fail(vpiError, std::format("vhpi_put_value: format {} is not supported",
                                   static_cast<int>(value->format)));
        return -1;
    }
    if (!valid) {
        fail(vpiError, std::format("vhpi_put_value: '{}' takes the positions of its {} literals",
                                   found->fullName, literals));
        return -1;
    }
    return vhpiWrite(*found, bits, mode) ? 0 : -1;
}

bool PliContext::vhpiWrite(const Object& object, const eir::BitVector& bits,
                           vhpiPutValueModeT mode) {
    // The kernel's own writes (section 9.4), as VPI's are. A deposit is an update of this time
    // step, which VPI's write with no delay is too: applied after the callback that made it
    // returns, so that what it wakes runs then rather than inside the call. A force or a release
    // is of the whole signal, which a part is forced within.
    switch (mode) {
    case vhpiDeposit:
    case vhpiDepositPropagate:
        if (object.isWindow)
            kernel->scheduleNonBlockingPart(object.signal, bits.ref(), object.lowBit);
        else
            kernel->scheduleNonBlocking(object.signal, bits.ref());
        return true;
    case vhpiForce:
    case vhpiForcePropagate: {
        Object whole;
        whole.signal = object.signal;
        whole.width = image->signals()[object.signal].bitWidth;
        eir::BitVector forced = readSignal(whole);
        eir::bitops::assignPartAt(forced.mutableRef(), bits.ref(), object.lowBit);
        kernel->force(object.signal, forced.ref());
        return true;
    }
    case vhpiRelease: kernel->release(object.signal); return true;
    default:
        fail(vpiError,
             std::format("vhpi_put_value: mode {} is not supported", static_cast<int>(mode)));
        return false;
    }
}

// ---------------------------------------------------------------------------
// Time, callbacks and control
// ---------------------------------------------------------------------------

void PliContext::vhpiGetTime(vhpiTimeT* time, long* cycles) {
    clearError();
    const runtime::SimTime now = kernel != nullptr ? kernel->time() : 0;
    if (time != nullptr) {
        time->high = static_cast<int32_t>(now >> 32U);
        time->low = static_cast<uint32_t>(now);
    }
    if (cycles != nullptr) *cycles = 0;
}

vhpiHandleT PliContext::vhpiRegisterCallback(vhpiCbDataT* data, std::int32_t flags) {
    clearError();
    if (data == nullptr || data->cb_rtn == nullptr) {
        fail(vpiError, "vhpi_register_cb: the callback data or its routine is a null pointer");
        return nullptr;
    }
    auto callback = std::make_unique<Callback>();
    callback->kind = Object::Kind::Callback;
    callback->type = vpiCallback;
    callback->vhpiData = *data;
    callback->vhpiData.time = nullptr;
    callback->vhpiData.value = nullptr;
    if (data->value != nullptr) callback->vhpiValueFormat = data->value->format;
    const runtime::SimTime now = kernel != nullptr ? kernel->time() : 0;
    Callback* raw = callback.get();

    switch (data->reason) {
    case vhpiCbValueChange: {
        const Object* subject = vhpiResolve(data->obj, "vhpi_register_cb");
        if (subject == nullptr) return nullptr;
        if (!hasValue(subject->vhpiClass) || subject->vhpiClass == vhpiGenericDeclK) {
            fail(vpiError, "vhpi_register_cb: the object has no value to change");
            return nullptr;
        }
        callback->signal = subject->signal;
        callback->width = subject->width;
        callback->isWindow = subject->isWindow;
        callback->lowBit = subject->lowBit;
        callback->instance = subject->instance;
        callback->vhdlType = subject->vhdlType;
        callback->name = subject->name;
        callback->fullName = subject->fullName;
        if (subject->isWindow) callback->lastValue = readSignal(*subject);
        valueChangeWatchers[subject->signal].push_back(raw);
        break;
    }
    case vhpiCbAfterDelay: {
        if (data->time == nullptr) {
            fail(vpiError, "vhpi_register_cb: vhpiCbAfterDelay needs a time");
            return nullptr;
        }
        const runtime::SimTime delay = (static_cast<runtime::SimTime>(data->time->high) << 32U)
                                       | static_cast<runtime::SimTime>(data->time->low);
        timedCallbacks.emplace(now + delay, raw);
        break;
    }
    // The last delta cycle of a time step is VPI's read-write synchronisation, the end of the
    // step its read-only one, and the next step its next simulation time (section 9.3); the
    // repetitive kinds fire at each occurrence until removed.
    case vhpiCbRepLastKnownDeltaCycle:
    case vhpiCbLastKnownDeltaCycle:
        callback->repeats = data->reason == vhpiCbRepLastKnownDeltaCycle;
        (phase == Phase::ReadOnly ? deferredReadWrite : readWrite).push_back(raw);
        break;
    case vhpiCbRepEndOfTimeStep:
    case vhpiCbEndOfTimeStep:
        callback->repeats = data->reason == vhpiCbRepEndOfTimeStep;
        (phase == Phase::ReadOnly ? deferredReadOnly : readOnly).push_back(raw);
        break;
    case vhpiCbRepNextTimeStep:
    case vhpiCbNextTimeStep:
        callback->repeats = data->reason == vhpiCbRepNextTimeStep;
        nextSimTime.push_back(raw);
        break;
    case vhpiCbStartOfSimulation: startCallbacks.push_back(raw); break;
    case vhpiCbEndOfSimulation: endCallbacks.push_back(raw); break;
    default:
        fail(vpiError,
             std::format("vhpi_register_cb: callback reason {} is not supported", data->reason));
        return nullptr;
    }

    objects.emplace(raw, std::move(callback));
    return (flags & vhpiReturnCb) != 0 ? reinterpret_cast<vhpiHandleT>(raw) : nullptr;
}

void PliContext::vhpiFire(Callback& callback) {
    vhpiCbDataT data = callback.vhpiData;
    vhpiTimeT time{};
    vhpiGetTime(&time, nullptr);
    data.time = &time;

    vhpiValueT value{.format = vhpiBinStrVal};
    std::vector<char> buffer;
    if (callback.vhpiData.reason == vhpiCbValueChange && callback.vhpiValueFormat.has_value()
        && kernel != nullptr) {
        value.format = *callback.vhpiValueFormat;
        // Sized by asking first, as an application would.
        const eir::BitVector bits = readSignal(callback);
        const int needed = vhpiFormat(callback, bits, value);
        if (needed > 0) {
            buffer.resize(static_cast<std::size_t>(needed));
            value.bufSize = buffer.size();
            value.value.ptr = buffer.data();
            static_cast<void>(vhpiFormat(callback, bits, value));
        }
        data.value = &value;
    }

    ++dispatchDepth;
    callback.vhpiData.cb_rtn(&data);
    --dispatchDepth;
}

int PliContext::vhpiRemoveCallback(vhpiHandleT handle) {
    clearError();
    Object* found = vhpiResolve(handle, "vhpi_remove_cb");
    auto* callback = dynamic_cast<Callback*>(found);
    if (callback == nullptr) {
        if (found != nullptr) fail(vpiError, "vhpi_remove_cb: the handle is not a callback");
        return -1;
    }
    retire(*callback);
    if (dispatchDepth == 0) sweepNow();
    return 0;
}

int PliContext::vhpiReleaseHandle(vhpiHandleT handle) {
    clearError();
    const auto found = objects.find(static_cast<const void*>(handle));
    if (handle == nullptr || found == objects.end()) return -1;
    // A callback stays registered when its handle is released; anything else is freed.
    if (found->second->kind == Object::Kind::Vhpi) objects.erase(found);
    return 0;
}

int PliContext::vhpiCompareHandles(vhpiHandleT first, vhpiHandleT second) {
    clearError();
    const Object* left = vhpiResolve(first, "vhpi_compare_handles");
    const Object* right = vhpiResolve(second, "vhpi_compare_handles");
    if (left == nullptr || right == nullptr) return 0;
    return left == right
                   || (left->vhpiClass == right->vhpiClass && left->fullName == right->fullName
                       && left->vhdlType == right->vhdlType && left->baseType == right->baseType)
               ? 1
               : 0;
}

int PliContext::vhpiCheckError(vhpiErrorInfoT* info) {
    if (errorLevel == 0) return 0;
    if (info != nullptr) {
        switch (errorLevel) {
        case vpiNotice: info->severity = vhpiNote; break;
        case vpiWarning: info->severity = vhpiWarning; break;
        case vpiSystem: info->severity = vhpiSystem; break;
        case vpiInternal: info->severity = vhpiInternal; break;
        default: info->severity = vhpiError; break;
        }
        info->message = errorMessage.data();
        info->str = errorCode.data();
        info->file = errorFile.data();
        info->line = 0;
    }
    return 1;
}

int PliContext::vhpiControl(vhpiSimControlT command) {
    clearError();
    switch (command) {
    case vhpiFinish: return control(vpiFinish) != 0 ? 0 : -1;
    case vhpiStop: return control(vpiStop) != 0 ? 0 : -1;
    default:
        fail(vpiError,
             std::format("vhpi_control: command {} is not supported", static_cast<int>(command)));
        return -1;
    }
}

int PliContext::vhpiPrint(std::string_view text) {
    return print(text);
}

} // namespace enzan::pli
