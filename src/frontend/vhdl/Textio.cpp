// SPDX-License-Identifier: GPL-3.0-or-later
//
// std.textio and VHDL's files, run natively by the runtime.
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

/// True when `type` is std.textio's `text`, whose files hold lines of characters.
bool isTextFile(type_t type) {
    type_t base = type_base_recur(type);
    return type_has_ident(base) && std::string_view(istr(type_ident(base))) == "STD.TEXTIO.TEXT";
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

void BodyTranslator::closeDeclaredFiles(tree_t declarer) {
    for (unsigned index = 0; index < tree_decls(declarer); ++index) {
        tree_t declaration = tree_decl(declarer, index);
        if (tree_kind(declaration) != T_FILE_DECL) continue;
        const auto local = locals.find(declaration);
        if (local == locals.end()) continue;
        const Value file = local->second;
        const Value none = constant(0, file.type, nullptr);
        const std::uint32_t open = compare(eir::Opcode::NotEqual, file, none);
        const std::uint32_t close = newBlock("file-close");
        const std::uint32_t closed = newBlock("file-closed");
        emit(eir::Instruction::make(eir::Opcode::BranchIf, {}, eir::Operand::reg(open),
                                    eir::Operand::block(close), eir::Operand::block(closed)));
        selectBlock(close);
        eir::SysTaskCall call;
        call.kind = eir::SysTaskKind::FileClose;
        call.location = owner.locate(declaration);
        call.arguments.push_back(
            eir::SysTaskArgument{.value = eir::Operand::reg(file.reg), .type = file.type});
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(call)))));
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(file.reg),
                                    eir::Operand::reg(none.reg)));
        jump(closed);
        selectBlock(closed);
    }
}

std::optional<BodyTranslator::Value>
BodyTranslator::writeBuiltString(tree_t built, const Value& line, const Value& justified,
                                 const Value& field, SourceLocation location) {
    const eir::Type handle = eir::Type::bits(64, false, false);
    const Value empty = constant(0, handle, nullptr);
    const std::uint32_t sink = newRegister(handle);
    emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(sink),
                                eir::Operand::reg(empty.reg)));
    if (!appendToLine(built, sink)) return std::nullopt;

    eir::SysTaskCall append;
    append.kind = eir::SysTaskKind::LineAppend;
    append.location = location;
    const auto argument = [](const Value& from) {
        return eir::SysTaskArgument{.value = eir::Operand::reg(from.reg), .type = from.type};
    };
    append.arguments = {argument(line), argument(justified), argument(field),
                        eir::SysTaskArgument{.value = eir::Operand::reg(sink), .type = handle}};
    return systemFunction(std::move(append), handle);
}

bool BodyTranslator::isBuiltString(tree_t expression) {
    while (tree_kind(expression) == T_QUALIFIED || tree_kind(expression) == T_TYPE_CONV)
        expression = tree_value(expression);
    type_t type = tree_type(expression);
    return tree_kind(expression) == T_FCALL && tree_has_ref(expression)
           && hasVhdlBody(static_cast<subprogram_kind_t>(tree_subkind(tree_ref(expression))))
           && type_is_array(type) && isCharacters(type) && !layoutFor(type)
           && !folder().boundsOf(expression);
}

bool BodyTranslator::appendToLine(tree_t expression, std::uint32_t sink) {
    // The pieces of a concatenation in order, as NVC's simplification leaves a chain of `&`.
    std::vector<tree_t> pieces;
    const auto flatten = [&](const auto& self, tree_t piece) -> void {
        while (tree_kind(piece) == T_QUALIFIED || tree_kind(piece) == T_TYPE_CONV)
            piece = tree_value(piece);
        if (tree_kind(piece) == T_AGGREGATE) {
            bool concatenation = true;
            for (unsigned index = 0; index < tree_assocs(piece); ++index) {
                const unsigned kind = tree_subkind(tree_assoc(piece, index));
                concatenation = concatenation && (kind == A_CONCAT || kind == A_POS);
            }
            if (concatenation && !isCharacters(tree_type(piece))) concatenation = false;
            if (concatenation) {
                for (unsigned index = 0; index < tree_assocs(piece); ++index)
                    self(self, tree_value(tree_assoc(piece, index)));
                return;
            }
        }
        if (tree_kind(piece) == T_FCALL && tree_has_ref(piece)
            && tree_subkind(tree_ref(piece)) == S_CONCAT && tree_params(piece) == 2) {
            self(self, tree_value(tree_param(piece, 0)));
            self(self, tree_value(tree_param(piece, 1)));
            return;
        }
        pieces.push_back(piece);
    };
    flatten(flatten, expression);

    // What a message would make of them, written as one; a string whose length only the run
    // shows is written by its function, inlined into the same line, between them.
    const eir::Type handle = eir::Type::bits(64, false, false);
    std::string format;
    std::vector<eir::SysTaskArgument> arguments;
    const auto flush = [&] {
        if (format.empty()) return;
        eir::SysTaskCall write;
        write.kind = eir::SysTaskKind::LineWrite;
        write.location = statementLocation;
        const Value right = constant(0, eir::Type::bits(8, false, false), nullptr);
        const Value width = constant(0, eir::Type::bits(32, false, false), nullptr);
        write.arguments = {
            eir::SysTaskArgument{.value = eir::Operand::reg(sink), .type = handle},
            eir::SysTaskArgument{.value = eir::Operand::reg(right.reg), .type = right.type},
            eir::SysTaskArgument{.value = eir::Operand::reg(width.reg), .type = width.type}};
        write.arguments.insert(write.arguments.end(), arguments.begin(), arguments.end());
        write.formatIndex = addString(std::exchange(format, {}));
        arguments.clear();
        const Value written = systemFunction(std::move(write), handle);
        releaseMessageScratch();
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(sink),
                                    eir::Operand::reg(written.reg)));
    };
    for (tree_t piece : pieces) {
        if (!isBuiltString(piece)) {
            if (!messageParts(piece, format, arguments)) return false;
            continue;
        }
        flush();
        tree_t made = piece;
        while (tree_kind(made) == T_QUALIFIED || tree_kind(made) == T_TYPE_CONV)
            made = tree_value(made);
        tree_t declaration = tree_ref(made);
        tree_t body = subprogramBody(declaration);
        const std::optional<std::vector<tree_t>> actuals =
            body == nullptr ? std::nullopt : actualsOf(made, declaration);
        if (!actuals) return fail(made, "a call of a function whose body has not been analysed");
        nextLineSink = sink;
        const std::optional<Value> inlinedCall = inlineCall(made, declaration, body, *actuals);
        nextLineSink.reset();
        if (!inlinedCall) return false;
    }
    flush();
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

std::optional<BodyTranslator::Value> BodyTranslator::lineLength(tree_t attribute) {
    type_t type = tree_type(attribute);
    const std::optional<eir::Type> storage = scalarStorage(type);
    if (!storage) {
        fail(attribute, "this attribute of a line");
        return std::nullopt;
    }
    const std::optional<Value> line = expression(tree_value(tree_name(attribute)));
    if (!line) return std::nullopt;
    eir::SysTaskCall function;
    function.kind = eir::SysTaskKind::LineLength;
    function.location = owner.locate(attribute);
    function.arguments.push_back(
        eir::SysTaskArgument{.value = eir::Operand::reg(line->reg), .type = line->type});
    Value length = resize(systemFunction(std::move(function), eir::Type::bits(32, false, false)),
                          storage->bitWidth());
    length.vhdlType = type;
    return length;
}

std::optional<BodyTranslator::Value> BodyTranslator::linePart(tree_t expression, type_t type) {
    const std::optional<Value> line = this->expression(tree_value(tree_value(expression)));
    if (!line) return std::nullopt;
    eir::SysTaskCall function;
    function.location = owner.locate(expression);
    function.arguments.push_back(
        eir::SysTaskArgument{.value = eir::Operand::reg(line->reg), .type = line->type});
    const eir::Type position = eir::Type::bits(64, true, false);

    if (tree_kind(expression) == T_ARRAY_REF) {
        const std::optional<eir::Type> storage = scalarStorage(type);
        if (tree_params(expression) != 1 || !storage) {
            fail(expression, "this element of a line");
            return std::nullopt;
        }
        const std::optional<Value> index = this->expression(tree_value(tree_param(expression, 0)));
        if (!index) return std::nullopt;
        const Value at = resize(*index, position.bitWidth());
        function.kind = eir::SysTaskKind::LineElement;
        function.arguments.push_back(
            eir::SysTaskArgument{.value = eir::Operand::reg(at.reg), .type = position});
        Value character =
            resize(systemFunction(std::move(function), eir::Type::bits(8, false, false)),
                   storage->bitWidth());
        character.vhdlType = type;
        return character;
    }

    // A slice's bounds fold where they are counted from the line's start, as `l(l'low to l'low +
    // 2)` is; its length then does.
    const std::optional<Folder::Range> bounds = folder().range(tree_range(expression, 0));
    if (!bounds || !bounds->ascending || bounds->length() <= 0) {
        fail(expression, "a slice of a line other than one rising over static bounds");
        return std::nullopt;
    }
    const std::optional<Layout> layout = arrayLayout(*bounds, type_elem(type), folder());
    if (!layout) {
        fail(expression, "this slice of a line");
        return std::nullopt;
    }
    function.kind = eir::SysTaskKind::LineSlice;
    function.arguments.push_back(eir::SysTaskArgument{
        .value = eir::Operand::reg(constant(bounds->left, position, nullptr).reg),
        .type = position});
    Value slice = systemFunction(std::move(function), layout->storage);
    slice.vhdlType = type;
    slice.layout = *layout;
    return slice;
}

std::optional<bool> BodyTranslator::nativeProcedure(tree_t call, tree_t declaration) {
    const auto kind = static_cast<subprogram_kind_t>(tree_subkind(declaration));
    const bool predefinedFile = kind == S_FILE_OPEN1 || kind == S_FILE_OPEN2 || kind == S_FILE_CLOSE
                                || kind == S_DEALLOCATE || kind == S_FILE_READ
                                || kind == S_FILE_WRITE;
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

    // `write(f, value)` and `read(f, value)` of a file of a type: its values' bits, in Enzan's own
    // format, which a run reads back as it wrote it.
    case S_FILE_WRITE: {
        const std::optional<Value> file = fileDescriptor((*actuals)[0]);
        if (!file) return false;

        // std.textio's `text`, `output` among its files, is written as the characters themselves,
        // as a message is made of them; any other file of strings as values of its type are.
        if (isTextFile(tree_type((*actuals)[0]))) {
            std::string format;
            native.kind = eir::SysTaskKind::FileWrite;
            native.arguments = {argument(*file)};
            if (!messageParts((*actuals)[1], format, native.arguments)) return false;
            native.formatIndex = addString(std::move(format));
            emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                        eir::Operand::sysCall(addSysCall(std::move(native)))));
            releaseMessageScratch();
            return true;
        }

        const std::optional<Value> written = value(1);
        if (!written) return false;
        native.kind = eir::SysTaskKind::FileWriteValue;
        native.arguments = {argument(*file), argument(*written)};
        // An unconstrained array's values differ in length, so each goes with its count.
        if (type_is_unconstrained(type_designated(tree_type((*actuals)[0])))) {
            if (!written->layout.isArray())
                return fail(call, "a write of this value to a file of arrays");
            native.arguments.push_back(argument(constant(
                written->layout.index.length(), eir::Type::bits(32, false, false), nullptr)));
        }
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(native)))));
        return true;
    }
    case S_FILE_READ: {
        const std::optional<Value> file = fileDescriptor((*actuals)[0]);
        const std::optional<Place> target = place((*actuals)[1]);
        if (!file || !target) return false;
        native.kind = eir::SysTaskKind::FileReadValue;
        native.arguments = {argument(*file)};
        const std::uint32_t width = target->layout.storage.bitWidth();
        if (actuals->size() == 2) {
            const Value read =
                systemFunction(std::move(native), eir::Type::bits(width, false, false));
            return assignWhole((*actuals)[1], read);
        }

        // `read(f, value, length)` of an unconstrained array: the value's leftmost elements, as
        // many as fit, over the array's own and, above them, how many there are (IEEE 1076
        // section 5.5.2), which is what GHDL gives.
        if (!target->layout.isArray() || !target->layout.inner.empty())
            return fail(call, "a read of this value from a file of arrays");
        const std::optional<Value> before = value(1);
        if (!before) return false;
        native.arguments.push_back(argument(
            constant(target->layout.elementBits, eir::Type::bits(32, false, false), nullptr)));
        native.arguments.push_back(argument(*before));
        const Value read =
            systemFunction(std::move(native), eir::Type::bits(width + 32, false, false));
        if (!assignWhole((*actuals)[1], bitsOf(read, 0, width))) return false;
        return assignWhole((*actuals)[2], bitsOf(read, width, 32));
    }

    // A closed file holds no descriptor, so that closing it as its subprogram returns does not
    // close it again.
    case S_FILE_CLOSE: {
        const std::optional<Value> file = fileDescriptor((*actuals)[0]);
        if (!file) return false;
        native.kind = eir::SysTaskKind::FileClose;
        native.arguments.push_back(argument(*file));
        emit(eir::Instruction::make(eir::Opcode::SysTask, {},
                                    eir::Operand::sysCall(addSysCall(std::move(native)))));
        emit(eir::Instruction::make(eir::Opcode::Move, eir::Operand::reg(file->reg),
                                    eir::Operand::reg(constant(0, file->type, nullptr).reg)));
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
        if (!line || !justified || !field) return false;

        // A string whose length only the run shows, a function's result built from images, is
        // built in a line of its own.
        if (isBuiltString(written)) {
            const std::optional<Value> appended =
                writeBuiltString(written, *line, *justified, *field, native.location);
            return appended && assignWhole((*actuals)[0], *appended);
        }
        // One whose length only the run knows is shown from the kernel holding it, a slice's
        // copied there for the write.
        if (runTimeString(written) != nullptr || isRunTimeSlice(written)) {
            std::optional<Value> mark;
            const std::optional<Value> string = scratchActual(written, mark);
            if (!string) return false;
            native.kind = eir::SysTaskKind::LineWrite;
            native.formatIndex = addString("%s");
            native.arguments = {argument(*line), argument(*justified), argument(*field),
                                eir::SysTaskArgument{.value = eir::Operand::reg(string->reg),
                                                     .type = eir::Type::string()}};
            const Value appended = systemFunction(std::move(native), handle);
            if (mark) releaseScratch(*mark);
            return assignWhole((*actuals)[0], appended);
        }
        const std::optional<Value> shown = value(1);
        if (!shown) return false;
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

    // `read(l, value)` and `read(l, value, good)` of a character; with no `good` to tell, an empty
    // line ends the run with a failure, as GHDL's textio does.
    if (procedure == "READ" && (actuals->size() == 2 || actuals->size() == 3)
        && isCharacters(tree_type((*actuals)[1])) && !type_is_array(tree_type((*actuals)[1]))) {
        const std::optional<Value> line = value(0);
        if (!line) return false;
        native.kind = eir::SysTaskKind::LineReadCharacter;
        native.arguments = {argument(*line)};
        if (actuals->size() == 2)
            native.arguments.push_back(
                argument(constant(1, eir::Type::bits(1, false, false), nullptr)));
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

    // `read(l, value)` and `read(l, value, good)` of a string: as many characters as it holds,
    // where the line has them. A null one takes none and is always read.
    if (procedure == "READ" && (actuals->size() == 2 || actuals->size() == 3)
        && type_is_array(tree_type((*actuals)[1])) && isCharacters(tree_type((*actuals)[1]))) {
        const Value failing = constant(1, eir::Type::bits(1, false, false), nullptr);
        // Into a string whose length only the run knows, which the kernel holds, or a slice of
        // one known here whose length the run gives, through a copy there written back.
        if (runTimeString((*actuals)[1]) != nullptr || isRunTimeSlice((*actuals)[1])) {
            const std::optional<Value> line = value(0);
            if (!line) return false;
            std::optional<Value> mark;
            const std::optional<Value> string = scratchActual((*actuals)[1], mark);
            if (!string) return false;
            native.kind = eir::SysTaskKind::StringRead;
            native.arguments = {argument(*string), argument(*line)};
            if (actuals->size() == 2) native.arguments.push_back(argument(failing));
            const Value read = systemFunction(std::move(native), eir::Type::bits(1, false, false));
            if (mark) {
                if (!sliceFromScratch((*actuals)[1], *string)) return false;
                releaseScratch(*mark);
            }
            if (actuals->size() == 2) return true;
            return assignWhole((*actuals)[2], booleanOf(read.reg, tree_type((*actuals)[2])));
        }
        const std::optional<Place> target = place((*actuals)[1]);
        if (!target) return false;
        if (!target->isVariable) return fail((*actuals)[1], "a read into something not a variable");
        Value good = constant(1, eir::Type::bits(1, false, false), nullptr);
        if (!target->empty) {
            const std::optional<Value> line = value(0);
            if (!line) return false;
            const Value before = read(*target, tree_type((*actuals)[1]));
            const std::uint32_t width = target->layout.storage.bitWidth();
            native.kind = eir::SysTaskKind::LineReadString;
            native.arguments = {argument(*line), argument(before)};
            if (actuals->size() == 2) native.arguments.push_back(argument(failing));
            const Value taken =
                systemFunction(std::move(native), eir::Type::bits(width + 1, false, false));
            assignVariable(*target, bitsOf(taken, 0, width));
            good = bitsOf(taken, width, 1);
        }
        if (actuals->size() == 2) return true;
        return assignWhole((*actuals)[2], booleanOf(good.reg, tree_type((*actuals)[2])));
    }

    return fail(call, std::string("std.textio's ") + simpleName(tree_ident(declaration))
                          + " of these parameters");
}

} // namespace enzan::frontend::vhdl
