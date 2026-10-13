// SPDX-License-Identifier: GPL-3.0-or-later
//
// std.textio and VHDL's files, run natively by the runtime (DESIGN_GUIDE.md section 4.3).
//
// NVC's textio body grows a line through access values to strings of run-time length, which EIR's
// registers of fixed width cannot hold. So its subprograms are not translated: a line is a handle
// into the runtime's heap, and each of `write`, `writeline`, `readline` and `read` is a system
// function that works on it, as NVC's runtime works on its foreign ones.

#include "frontend/vhdl/BodyTranslator.h"
#include "frontend/vhdl/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// Descriptors of the standard streams, as the runtime numbers them.
constexpr std::uint32_t StandardInput = 0x8000'0000U;
constexpr std::uint32_t StandardOutput = 0x8000'0001U;

/// True when `declaration` is declared by std.textio.
bool inTextio(tree_t declaration) {
    tree_t unit = tree_container(declaration);
    return unit != nullptr && tree_kind(unit) == T_PACKAGE
           && std::string_view(istr(tree_ident(unit))) == "STD.TEXTIO";
}

/// True when `type` is a string, or a character.
bool isCharacters(type_t type) {
    type_t element = type_is_array(type) ? type_elem(type) : type;
    return type_is_enum(element)
           && std::string_view(istr(type_ident(type_base_recur(element))))
                  == "STD.STANDARD.CHARACTER";
}

} // namespace

BodyTranslator::Value BodyTranslator::systemFunction(eir::SysTaskCall call, eir::Type type) {
    const std::uint32_t reg = newRegister(type);
    emit(eir::Instruction::make(eir::Opcode::SysFunction, eir::Operand::reg(reg),
                                eir::Operand::sysCall(addSysCall(std::move(call)))));
    return Value{.reg = reg, .type = type, .vhdlType = nullptr};
}

std::optional<BodyTranslator::Value> BodyTranslator::fileDescriptor(tree_t name) {
    if (tree_kind(name) != T_REF || !tree_has_ref(name)) {
        fail(name, "a file named other than by its declaration");
        return std::nullopt;
    }
    tree_t declaration = tree_ref(name);
    if (inTextio(declaration)) {
        const std::string_view file = istr(tree_ident(declaration));
        const eir::Type word = eir::Type::bits(32, false, false);
        if (file == "OUTPUT") return constant(StandardOutput, word, nullptr);
        if (file == "INPUT") return constant(StandardInput, word, nullptr);
    }
    if (const auto local = locals.find(declaration); local != locals.end()) return local->second;
    fail(name, "a file not declared by the process");
    return std::nullopt;
}

BodyTranslator::Value BodyTranslator::openFile(const Value& name, std::int64_t mode, tree_t where) {
    // `file_open_kind`'s positions: read_mode, write_mode, append_mode.
    char letter = 'r';
    if (mode == 1) letter = 'w';
    if (mode == 2) letter = 'a';
    eir::SysTaskCall call;
    call.kind = eir::SysTaskKind::FileOpen;
    call.location = owner.locate(where);
    const Value modeText =
        constant(static_cast<unsigned char>(letter), eir::Type::bits(8, false, false), nullptr);
    call.arguments = {
        eir::SysTaskArgument{.value = eir::Operand::reg(name.reg), .type = name.type},
        eir::SysTaskArgument{.value = eir::Operand::reg(modeText.reg), .type = modeText.type}};
    return systemFunction(std::move(call), eir::Type::bits(32, false, false));
}

bool BodyTranslator::declareFile(tree_t declaration) {
    const eir::Type word = eir::Type::bits(32, false, false);
    const Value descriptor = constant(0, word, nullptr);
    const Value file{.reg = newRegister(word), .type = word, .vhdlType = tree_type(declaration)};
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(file.reg),
                                eir::Operand::reg(descriptor.reg)));
    locals[declaration] = file;
    if (!tree_has_value(declaration)) return true;

    // `file f : text open write_mode is name;` opens it as the declaration elaborates.
    std::optional<std::int64_t> mode = std::int64_t{0};
    if (tree_t given = tree_file_mode(declaration); given != nullptr)
        mode = folder().integer(given);
    const std::optional<Value> name = expression(tree_value(declaration));
    if (!mode || !name) return fail(declaration, "a file opened with a mode that is not static");
    const Value opened = openFile(*name, *mode, declaration);
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(file.reg),
                                eir::Operand::reg(opened.reg)));
    return true;
}

bool BodyTranslator::assignWhole(tree_t name, const Value& value) {
    const std::optional<Place> target = place(name);
    if (!target) return false;
    if (!target->isVariable || !target->whole)
        return fail(name, "a line other than a whole variable");
    emit(eir::Instruction::make(
        eir::Opcode::Move, eir::Operand::reg(target->variable.reg),
        eir::Operand::reg(resize(value, target->variable.type.bitWidth()).reg)));
    return true;
}

std::optional<BodyTranslator::Value> BodyTranslator::endOfFile(tree_t call) {
    if (tree_params(call) != 1) {
        fail(call, "this endfile");
        return std::nullopt;
    }
    const std::optional<Value> file = fileDescriptor(tree_value(tree_param(call, 0)));
    if (!file) return std::nullopt;
    eir::SysTaskCall function;
    function.kind = eir::SysTaskKind::FileEnd;
    function.location = owner.locate(call);
    function.arguments.push_back(
        eir::SysTaskArgument{.value = eir::Operand::reg(file->reg), .type = file->type});
    const Value ended = systemFunction(std::move(function), eir::Type::bits(1, false, false));
    return booleanOf(ended.reg, tree_type(call));
}

std::optional<bool> BodyTranslator::nativeProcedure(tree_t call, tree_t declaration) {
    const auto kind = static_cast<subprogram_kind_t>(tree_subkind(declaration));
    const bool predefinedFile = kind == S_FILE_OPEN1 || kind == S_FILE_OPEN2 || kind == S_FILE_CLOSE
                                || kind == S_DEALLOCATE;
    if (!predefinedFile && !inTextio(declaration)) return std::nullopt;

    const std::optional<std::vector<tree_t>> actuals = actualsOf(call, declaration);
    if (!actuals) return false;
    const auto value = [&](std::size_t index) { return expression((*actuals)[index]); };
    const auto argument = [](const Value& from) {
        return eir::SysTaskArgument{.value = eir::Operand::reg(from.reg), .type = from.type};
    };
    eir::SysTaskCall native;
    native.location = owner.locate(call);
    const eir::Type handle = eir::Type::bits(64, false, false);

    switch (kind) {
    // `deallocate(l)`, of a line: the handle is null after.
    case S_DEALLOCATE: {
        const std::optional<Value> line = value(0);
        if (!line) return false;
        if (!isCharacters(type_designated(tree_type((*actuals)[0]))))
            return fail(call, "deallocating anything but a line");
        native.kind = eir::SysTaskKind::LineDeallocate;
        native.arguments.push_back(argument(*line));
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(native)))));
        return assignWhole((*actuals)[0], constant(0, handle, nullptr));
    }

    // `file_open(f, name, mode)`; the form with a status is not run yet.
    case S_FILE_OPEN1: {
        const std::optional<std::int64_t> mode =
            actuals->size() > 2 ? folder().integer((*actuals)[2]) : std::optional(std::int64_t{0});
        const std::optional<Value> name = value(1);
        if (!name) return false;
        if (!mode) return fail(call, "a file opened with a mode that is not static");
        return assignWhole((*actuals)[0], openFile(*name, *mode, call));
    }
    case S_FILE_OPEN2: return fail(call, "a file_open with a status");

    case S_FILE_CLOSE: {
        const std::optional<Value> file = fileDescriptor((*actuals)[0]);
        if (!file) return false;
        native.kind = eir::SysTaskKind::FileClose;
        native.arguments.push_back(argument(*file));
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(native)))));
        return true;
    }

    default: break;
    }

    const std::string_view procedure = istr(tree_ident(declaration));

    // `write(l, value, justified, field)`, of a string, a character or an integer.
    if (procedure == "WRITE" && actuals->size() == 4) {
        tree_t written = (*actuals)[1];
        type_t type = tree_type(written);
        const bool characters = isCharacters(type);
        if (!characters && !(type_is_integer(type) && isSignedScalar(type)))
            return fail(call, std::string("a write of ") + type_pp(type));
        const std::optional<Value> line = value(0);
        const std::optional<Value> justified = value(2);
        const std::optional<Value> field = value(3);
        const std::optional<Value> shown = value(1);
        if (!line || !justified || !field || !shown) return false;
        native.kind = eir::SysTaskKind::LineWrite;
        native.formatIndex = addString(characters ? "%s" : "%0d");
        native.arguments = {argument(*line), argument(*justified), argument(*field),
                            argument(*shown)};
        return assignWhole((*actuals)[0], systemFunction(std::move(native), handle));
    }

    // `writeline(f, l)` and `readline(f, l)`: the line is a new one after.
    if ((procedure == "WRITELINE" || procedure == "READLINE") && actuals->size() == 2) {
        const std::optional<Value> file = fileDescriptor((*actuals)[0]);
        const std::optional<Value> line = value(1);
        if (!file || !line) return false;
        native.kind = procedure == "WRITELINE" ? eir::SysTaskKind::LineWriteLine
                                               : eir::SysTaskKind::LineReadLine;
        native.arguments = {argument(*file), argument(*line)};
        return assignWhole((*actuals)[1], systemFunction(std::move(native), handle));
    }

    // `read(l, value)` and `read(l, value, good)` of a character.
    if (procedure == "READ" && (actuals->size() == 2 || actuals->size() == 3)
        && isCharacters(tree_type((*actuals)[1])) && !type_is_array(tree_type((*actuals)[1]))) {
        const std::optional<Value> line = value(0);
        if (!line) return false;
        native.kind = eir::SysTaskKind::LineReadCharacter;
        native.arguments = {argument(*line)};
        const Value read = systemFunction(std::move(native), eir::Type::bits(16, false, false));
        const auto bits = [&](std::uint32_t low, std::uint32_t width) {
            const Value part{.reg = newRegister(eir::Type::bits(width, false, false)),
                             .type = eir::Type::bits(width, false, false),
                             .vhdlType = nullptr};
            emit(eir::Instruction::make(
                eir::Opcode::PartSelect, eir::Operand::reg(part.reg), eir::Operand::reg(read.reg),
                eir::Operand::reg(constant(low, eir::Type::bits(32, false, false), nullptr).reg)));
            return part;
        };
        if (!assignWhole((*actuals)[1], bits(0, 8))) return false;
        if (actuals->size() == 3) {
            const Value good = booleanOf(bits(8, 1).reg, tree_type((*actuals)[2]));
            if (!assignWhole((*actuals)[2], good)) return false;
        }
        return true;
    }

    return fail(call, std::string("std.textio's ") + simpleName(tree_ident(declaration))
                          + " of these parameters");
}

} // namespace enzan::frontend::vhdl
