// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Folder.h"

#include "eir/BitOps.h"
#include "frontend/vhdl/Types.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// A real's code is its IEEE 754 double's bits (DESIGN_GUIDE.md section 3.6), which is how the
/// Folder carries one where it carries every other scalar.
std::int64_t realCode(double value) {
    return std::bit_cast<std::int64_t>(value);
}

double realOf(std::int64_t code) {
    return std::bit_cast<double>(code);
}

/// `value` rounded to the nearest integer, halfway away from zero, or nothing when no 64-bit
/// integer is it.
std::optional<std::int64_t> roundedReal(double value) {
    const double rounded = std::round(value);
    if (std::isnan(rounded) || rounded < -0x1p63 || rounded >= 0x1p63) return std::nullopt;
    return static_cast<std::int64_t>(rounded);
}

} // namespace

bool isForeign(tree_t declaration) {
    ident_t foreign = ident_new("FOREIGN");
    ident_t name = tree_ident2(declaration);
    const auto specifies = [foreign, name](tree_t region) {
        if (region == nullptr) return false;
        for (unsigned index = 0; index < tree_decls(region); ++index) {
            tree_t specification = tree_decl(region, index);
            if (tree_kind(specification) != T_ATTR_SPEC || tree_ident(specification) != foreign
                || !tree_has_ref(specification))
                continue;
            tree_t named = tree_ref(specification);
            const tree_kind_t kind = tree_kind(named);
            if ((kind == T_FUNC_DECL || kind == T_FUNC_BODY || kind == T_PROC_DECL
                 || kind == T_PROC_BODY)
                && tree_ident2(named) == name)
                return true;
        }
        return false;
    };
    tree_t unit = tree_container(declaration);
    if (unit == nullptr) return false;
    switch (tree_kind(unit)) {
    case T_PACKAGE: return specifies(unit) || specifies(body_of(unit));
    case T_PACK_BODY:
        return specifies(unit) || (tree_has_primary(unit) && specifies(tree_primary(unit)));
    default: return specifies(unit);
    }
}

tree_t subprogramBody(tree_t declaration) {
    if (isForeign(declaration)) return nullptr;
    const tree_kind_t kind = tree_kind(declaration);
    if (kind == T_FUNC_BODY || kind == T_PROC_BODY) return declaration;

    // The body has the declaration's mangled name, in the package body when the declaration is in
    // a package, and beside the declaration otherwise.
    tree_t unit = tree_container(declaration);
    tree_t region = tree_kind(unit) == T_PACKAGE ? body_of(unit) : unit;
    if (region == nullptr) return nullptr;
    ident_t name = tree_ident2(declaration);
    for (unsigned index = 0; index < tree_decls(region); ++index) {
        tree_t candidate = tree_decl(region, index);
        const tree_kind_t candidateKind = tree_kind(candidate);
        if ((candidateKind == T_FUNC_BODY || candidateKind == T_PROC_BODY)
            && tree_ident2(candidate) == name)
            return candidate;
    }
    return nullptr;
}

tree_t constantValue(tree_t declaration) {
    const tree_kind_t kind = tree_kind(declaration);
    if (kind != T_CONST_DECL && kind != T_ALIAS) return nullptr;
    if (tree_has_value(declaration)) return tree_value(declaration);
    tree_t unit = tree_container(declaration);
    tree_t body = unit != nullptr && tree_kind(unit) == T_PACKAGE ? body_of(unit) : nullptr;
    if (kind != T_CONST_DECL || body == nullptr) return nullptr;
    for (unsigned index = 0; index < tree_decls(body); ++index) {
        tree_t full = tree_decl(body, index);
        if (tree_kind(full) == T_CONST_DECL && tree_ident(full) == tree_ident(declaration)
            && tree_has_value(full))
            return tree_value(full);
    }
    return nullptr;
}

namespace {

/// VHDL's `mod`, whose result takes the sign of its right operand (IEEE 1076 section 9.2.7),
/// where C++'s `%` is VHDL's `rem` and takes the sign of its left one.
std::int64_t modulo(std::int64_t left, std::int64_t right) {
    const std::int64_t remainder = left % right;
    return remainder != 0 && ((remainder < 0) != (right < 0)) ? remainder + right : remainder;
}

/// Whether the IEEE packages' warnings are reported, for the one translation this process runs.
bool ieeeWarningsReported = true;

/// The value of a deferred constant whose full declaration computes it with one of NVC's foreign
/// functions, which Enzan supplies (DESIGN_GUIDE.md section 5.2), or nothing for any other.
std::optional<std::int64_t> foreignConstant(tree_t declaration) {
    tree_t unit = tree_container(declaration);
    if (tree_kind(unit) != T_PACKAGE) return std::nullopt;
    const std::string name =
        std::string(istr(tree_ident(unit))) + "." + istr(tree_ident(declaration));

    // Whether numeric_std's warnings of metavalues and truncation are silenced: they are not, as
    // they are not by default in GHDL or in the IEEE packages themselves, unless asked.
    if (name == "NVC.SIM_PKG.IEEE_NO_WARNING") return ieeeWarningsReported ? 0 : 1;
    return std::nullopt;
}

std::optional<std::int64_t> power(std::int64_t base, std::int64_t exponent) {
    if (exponent < 0) return std::nullopt;
    std::int64_t result = 1;
    for (std::int64_t step = 0; step < exponent; ++step) {
        if (__builtin_mul_overflow(result, base, &result)) return std::nullopt;
    }
    return result;
}

} // namespace

void setIeeeWarnings(bool reported) noexcept {
    ieeeWarningsReported = reported;
}

std::optional<std::int64_t> Folder::integer(tree_t expression) const {
    switch (tree_kind(expression)) {
    case T_LITERAL:
        switch (tree_subkind(expression)) {
        case L_INT: return tree_ival(expression);
        case L_REAL: return realCode(tree_dval(expression));
        case L_NULL: return 0;
        case L_PHYSICAL: {
            // A literal NVC has not reduced to the primary unit still names the unit it counts.
            if (!tree_has_ref(expression)) return tree_ival(expression);
            tree_t unit = tree_ref(expression);
            if (tree_kind(unit) != T_UNIT_DECL) return std::nullopt;
            const std::optional<std::int64_t> scale = integer(tree_value(unit));
            std::int64_t value = 0;
            if (!scale || __builtin_mul_overflow(tree_ival(expression), *scale, &value))
                return std::nullopt;
            return value;
        }
        default: return std::nullopt;
        }

    case T_QUALIFIED: return integer(tree_value(expression));

    // Between discrete and physical types a conversion keeps the value; into a real it is the
    // nearest double, and out of one it rounds halfway away from zero (IEEE 1076 section
    // 9.3.6).
    case T_TYPE_CONV: {
        const std::optional<std::int64_t> value = integer(tree_value(expression));
        if (!value) return std::nullopt;
        const bool fromReal = type_is_real(tree_type(tree_value(expression)));
        const bool toReal = type_is_real(tree_type(expression));
        if (fromReal == toReal) return value;
        if (toReal) return realCode(static_cast<double>(*value));
        return roundedReal(realOf(*value));
    }

    case T_REF: return reference(expression);
    case T_FCALL: return call(expression);
    case T_ATTR_REF: return attribute(expression);
    case T_ARRAY_REF: return element(expression);

    // A field of a static record, a generic's or a constant's.
    case T_RECORD_REF: {
        const std::optional<eir::BitVector> bits = fieldBits(expression, *this);
        if (!bits || bits->width() > 64) return std::nullopt;
        const std::uint64_t raw = bits->ref().toUInt64();
        const std::uint32_t width = bits->width();
        if (!isSignedScalar(tree_type(expression)) || width == 64)
            return static_cast<std::int64_t>(raw);
        const std::uint64_t sign = std::uint64_t{1} << (width - 1);
        return static_cast<std::int64_t>((raw ^ sign) - sign);
    }

    default: return std::nullopt;
    }
}

std::optional<std::int64_t> Folder::realCall(subprogram_kind_t kind,
                                             const std::vector<std::int64_t>& operands) {
    // The predefined operators of a real type, on the doubles the codes are; a mixed one, a time
    // scaled by a real, does not fold.
    const auto real = [](double value) -> std::optional<std::int64_t> { return realCode(value); };
    const auto truth = [](bool value) -> std::optional<std::int64_t> { return value ? 1 : 0; };
    if (operands.size() == 1) {
        const double a = realOf(operands[0]);
        switch (kind) {
        case S_IDENTITY: return real(a);
        case S_NEGATE: return real(-a);
        case S_ABS: return real(std::fabs(a));
        default: return std::nullopt;
        }
    }
    if (operands.size() != 2) return std::nullopt;
    const double a = realOf(operands[0]);
    const double b = realOf(operands[1]);
    switch (kind) {
    case S_ADD: return real(a + b);
    case S_SUB: return real(a - b);
    case S_MUL: return real(a * b);
    case S_DIV:
        if (b == 0.0) return std::nullopt;
        return real(a / b);
    // A real raised to an integer power, whose operand is an integer's code.
    case S_EXP: return real(std::pow(a, static_cast<double>(operands[1])));
    case S_MINIMUM: return real(std::fmin(a, b));
    case S_MAXIMUM: return real(std::fmax(a, b));
    case S_SCALAR_EQ: return truth(a == b);
    case S_SCALAR_NEQ: return truth(a != b);
    case S_SCALAR_LT: return truth(a < b);
    case S_SCALAR_LE: return truth(a <= b);
    case S_SCALAR_GT: return truth(a > b);
    case S_SCALAR_GE: return truth(a >= b);
    default: return std::nullopt;
    }
}

std::optional<std::int64_t> Folder::reference(tree_t expression) const {
    if (!tree_has_ref(expression)) return std::nullopt;
    tree_t declaration = tree_ref(expression);
    switch (tree_kind(declaration)) {
    case T_ENUM_LIT: return tree_pos(declaration);

    // A deferred constant has its value in the package body, but for one that NVC's runtime
    // computes there, whose value Enzan supplies in its stead.
    case T_CONST_DECL: {
        if (tree_has_value(declaration)) return integer(tree_value(declaration));
        if (const std::optional<std::int64_t> foreign = foreignConstant(declaration))
            return foreign;
        tree_t value = constantValue(declaration);
        return value == nullptr ? std::nullopt : integer(value);
    }

    // A constant function's parameters and variables are bound while it runs.
    case T_GENERIC_DECL:
    case T_PARAM_DECL:
    case T_VAR_DECL: {
        const auto bound = generics.find(declaration);
        if (bound != generics.end()) return bound->second;
        return std::nullopt;
    }

    case T_ALIAS:
    case T_UNIT_DECL: return integer(tree_value(declaration));

    default: return std::nullopt;
    }
}

std::optional<std::int64_t> Folder::call(tree_t expression) const {
    if (!tree_has_ref(expression)) return std::nullopt;
    tree_t declaration = tree_ref(expression);
    const auto kind = static_cast<subprogram_kind_t>(tree_subkind(declaration));
    if (hasVhdlBody(kind)) return constantCall(expression, declaration);

    // Two arrays of different lengths are never equal, and two null arrays always are, whatever
    // their elements would be: the bounds decide, without the values.
    if ((kind == S_ARRAY_EQ || kind == S_ARRAY_NEQ) && tree_params(expression) == 2) {
        tree_t leftValue = tree_value(tree_param(expression, 0));
        tree_t rightValue = tree_value(tree_param(expression, 1));
        const std::optional<Range> left = boundsOf(leftValue);
        const std::optional<Range> right = boundsOf(rightValue);
        if (!left || !right) return std::nullopt;
        if (left->length() != right->length()) return kind == S_ARRAY_NEQ ? 1 : 0;
        if (left->length() == 0) return kind == S_ARRAY_EQ ? 1 : 0;

        // Of one length, they are equal when their elements are, left to right, which each lays
        // out from its highest bits: a string generic against a literal, as a generate condition
        // chooses an implementation by. A real's bits are not its value, 0.0 and -0.0 being equal.
        type_t element = type_elem(tree_type(leftValue));
        while (type_is_array(element)) element = type_elem(element);
        if (!type_is_discrete(element) && !type_is_physical(element)) return std::nullopt;
        const std::optional<Layout> leftLayout =
            arrayLayout(*left, type_elem(tree_type(leftValue)), *this);
        const std::optional<Layout> rightLayout =
            arrayLayout(*right, type_elem(tree_type(rightValue)), *this);
        if (!leftLayout || !rightLayout) return std::nullopt;
        const std::optional<eir::BitVector> leftBits = constantBits(leftValue, *leftLayout, *this);
        const std::optional<eir::BitVector> rightBits =
            constantBits(rightValue, *rightLayout, *this);
        if (!leftBits || !rightBits) return std::nullopt;
        return (*leftBits == *rightBits) == (kind == S_ARRAY_EQ) ? 1 : 0;
    }

    std::vector<std::int64_t> operands;
    const unsigned count = tree_params(expression);
    for (unsigned index = 0; index < count; ++index) {
        const std::optional<std::int64_t> operand =
            integer(tree_value(tree_param(expression, index)));
        if (!operand) return std::nullopt;
        operands.push_back(*operand);
    }
    if (count > 0 && type_is_real(tree_type(tree_value(tree_param(expression, 0)))))
        return realCall(kind, operands);

    std::int64_t result = 0;
    const auto unary = [&operands] { return operands.size() == 1; };
    const auto binary = [&operands] { return operands.size() == 2; };
    switch (kind) {
    case S_ADD:
        if (!binary() || __builtin_add_overflow(operands[0], operands[1], &result))
            return std::nullopt;
        return result;
    case S_SUB:
        if (!binary() || __builtin_sub_overflow(operands[0], operands[1], &result))
            return std::nullopt;
        return result;
    case S_MUL:
    case S_MUL_PI:
    case S_MUL_IP:
        if (!binary() || __builtin_mul_overflow(operands[0], operands[1], &result))
            return std::nullopt;
        return result;
    case S_DIV:
    case S_DIV_PI:
    case S_DIV_PP:
        if (!binary() || operands[1] == 0) return std::nullopt;
        return operands[0] / operands[1];
    case S_MOD:
        if (!binary() || operands[1] == 0) return std::nullopt;
        return modulo(operands[0], operands[1]);
    case S_REM:
        if (!binary() || operands[1] == 0) return std::nullopt;
        return operands[0] % operands[1];
    case S_EXP:
        if (!binary()) return std::nullopt;
        return power(operands[0], operands[1]);
    case S_NEGATE:
        if (!unary() || __builtin_sub_overflow(std::int64_t{0}, operands[0], &result))
            return std::nullopt;
        return result;
    case S_IDENTITY:
        if (!unary()) return std::nullopt;
        return operands[0];
    case S_ABS:
        if (!unary() || __builtin_sub_overflow(std::int64_t{0}, operands[0], &result))
            return std::nullopt;
        return operands[0] < 0 ? result : operands[0];

    // The result is a boolean's position.
    case S_SCALAR_EQ: return binary() ? std::optional(operands[0] == operands[1]) : std::nullopt;
    case S_SCALAR_NEQ: return binary() ? std::optional(operands[0] != operands[1]) : std::nullopt;
    case S_SCALAR_LT: return binary() ? std::optional(operands[0] < operands[1]) : std::nullopt;
    case S_SCALAR_LE: return binary() ? std::optional(operands[0] <= operands[1]) : std::nullopt;
    case S_SCALAR_GT: return binary() ? std::optional(operands[0] > operands[1]) : std::nullopt;
    case S_SCALAR_GE: return binary() ? std::optional(operands[0] >= operands[1]) : std::nullopt;

    // On `bit` and `boolean`, whose positions are 0 and 1.
    case S_SCALAR_AND: return binary() ? std::optional(operands[0] & operands[1]) : std::nullopt;
    case S_SCALAR_OR: return binary() ? std::optional(operands[0] | operands[1]) : std::nullopt;
    case S_SCALAR_XOR: return binary() ? std::optional(operands[0] ^ operands[1]) : std::nullopt;
    case S_SCALAR_NAND:
        return binary() ? std::optional((operands[0] & operands[1]) ^ 1) : std::nullopt;
    case S_SCALAR_NOR:
        return binary() ? std::optional((operands[0] | operands[1]) ^ 1) : std::nullopt;
    case S_SCALAR_XNOR:
        return binary() ? std::optional((operands[0] ^ operands[1]) ^ 1) : std::nullopt;
    case S_SCALAR_NOT: return unary() ? std::optional(operands[0] ^ 1) : std::nullopt;

    case S_MINIMUM:
        return binary() ? std::optional(std::min(operands[0], operands[1])) : std::nullopt;
    case S_MAXIMUM:
        return binary() ? std::optional(std::max(operands[0], operands[1])) : std::nullopt;

    default: return std::nullopt;
    }
}

std::optional<std::int64_t> Folder::attribute(tree_t expression) const {
    const auto kind = static_cast<attr_kind_t>(tree_subkind(expression));
    tree_t prefix = tree_name(expression);

    const auto parameter = [expression, this]() -> std::optional<std::int64_t> {
        if (tree_params(expression) == 0) return std::nullopt;
        return integer(tree_value(tree_param(expression, 0)));
    };

    switch (kind) {
    case ATTR_LEFT:
    case ATTR_RIGHT:
    case ATTR_LOW:
    case ATTR_HIGH:
    case ATTR_LENGTH:
    case ATTR_ASCENDING: {
        if (!tree_has_type(prefix)) return std::nullopt;
        // An array's attribute names a dimension, the first unless a parameter says otherwise.
        unsigned dimension = 0;
        if (tree_params(expression) > 0) {
            const std::optional<std::int64_t> given = parameter();
            if (!given || *given < 1) return std::nullopt;
            dimension = static_cast<unsigned>(*given - 1);
        }
        const std::optional<Range> folded = boundsOf(prefix, dimension);
        if (!folded) return std::nullopt;
        switch (kind) {
        case ATTR_LEFT: return folded->left;
        case ATTR_RIGHT: return folded->right;
        case ATTR_LOW: return folded->low();
        case ATTR_HIGH: return folded->high();
        case ATTR_ASCENDING: return folded->ascending ? 1 : 0;
        default: return folded->length();
        }
    }

    // Positions are already what a discrete value folds to, so these change nothing but the
    // type, and a physical value's position is its count of the primary unit.
    case ATTR_POS:
    case ATTR_VAL: return parameter();

    case ATTR_SUCC:
    case ATTR_PRED: {
        const std::optional<std::int64_t> value = parameter();
        if (!value) return std::nullopt;
        return kind == ATTR_SUCC ? *value + 1 : *value - 1;
    }

    default: return std::nullopt;
    }
}

std::optional<Folder::Range> Folder::range(tree_t range) const {
    if (range == nullptr || tree_kind(range) != T_RANGE) return std::nullopt;
    switch (tree_subkind(range)) {
    case RANGE_TO:
    case RANGE_DOWNTO: {
        const std::optional<std::int64_t> left = integer(tree_left(range));
        const std::optional<std::int64_t> right = integer(tree_right(range));
        if (!left || !right) return std::nullopt;
        return Range{.left = *left, .right = *right, .ascending = tree_subkind(range) == RANGE_TO};
    }

    // `x'range` and `x'reverse_range`: the range of what the prefix names.
    case RANGE_EXPR: {
        tree_t attribute = tree_value(range);
        if (tree_kind(attribute) != T_ATTR_REF) return std::nullopt;
        tree_t prefix = tree_name(attribute);
        if (!tree_has_type(prefix)) return std::nullopt;
        unsigned dimension = 0;
        if (tree_params(attribute) > 0) {
            const std::optional<std::int64_t> given = integer(tree_value(tree_param(attribute, 0)));
            if (!given || *given < 1) return std::nullopt;
            dimension = static_cast<unsigned>(*given - 1);
        }
        std::optional<Range> folded = boundsOf(prefix, dimension);
        if (folded && tree_subkind(attribute) == ATTR_REVERSE_RANGE) {
            std::swap(folded->left, folded->right);
            folded->ascending = !folded->ascending;
        }
        return folded;
    }

    default: return std::nullopt;
    }
}

std::optional<Folder::Range> Folder::boundsOf(tree_t name, unsigned dimension) const {
    if (dimension == 0 && tree_kind(name) == T_REF && tree_has_ref(name)) {
        const auto bound = objectBounds.find(tree_ref(name));
        if (bound != objectBounds.end()) return bound->second;
    }
    // An element of an object whose elements take their bounds from its context, `a(i)`.
    if (dimension == 0 && tree_kind(name) == T_ARRAY_REF && tree_params(name) == 1) {
        tree_t prefix = tree_value(name);
        if (tree_kind(prefix) == T_REF && tree_has_ref(prefix)) {
            const auto bound = elementBounds.find(tree_ref(prefix));
            if (bound != elementBounds.end()) return bound->second;
        }
    }
    if (std::optional<Range> own =
            tree_has_type(name) ? rangeOf(tree_type(name), dimension) : std::nullopt)
        return own;

    // A constant of an unconstrained type has its value's bounds, and a function's result those
    // of what it returns.
    if (tree_kind(name) == T_REF && tree_has_ref(name)) {
        tree_t declaration = tree_ref(name);
        if (tree_kind(declaration) != T_CONST_DECL) return std::nullopt;
        if (tree_t value = constantValue(declaration)) return boundsOf(value, dimension);
        return std::nullopt;
    }
    std::optional<Range> result;
    if (tree_kind(name) == T_FCALL) {
        returned(name, [&result, dimension](tree_t value, const Folder& frame) {
            result = frame.boundsOf(value, dimension);
            return result.has_value();
        });
    }
    return result;
}

std::optional<Folder::Range> Folder::rangeOf(type_t type, unsigned dimension) const {
    if (type == nullptr) return std::nullopt;
    if (type_is_array(type)) {
        // The nearest index constraint among the subtypes it is declared through; an array type
        // with none is unconstrained, and has no range of its own.
        type_t constrained = type;
        while (type_kind(constrained) == T_SUBTYPE && !type_has_constraint(constrained))
            constrained = type_base(constrained);
        if (type_kind(constrained) != T_SUBTYPE) return std::nullopt;
        if (dimension >= dimension_of(type)) return std::nullopt;
        return range(range_of(constrained, dimension));
    }
    if (!type_is_scalar(type) || dimension != 0) return std::nullopt;
    if (dimension_of(type) == 0) return std::nullopt;
    return range(range_of(type, 0));
}

std::optional<std::int64_t> Folder::left(type_t type) const {
    const std::optional<Range> folded = rangeOf(type);
    if (folded) return folded->left;
    // An enumeration with no explicit range starts at its first literal.
    if (type_is_enum(type)) return 0;
    return std::nullopt;
}

// --- constant functions -----------------------------------------------------

namespace {

/// How a statement of a constant function ended.
enum class Flow : std::uint8_t { Continue, Return, Exit, Next, Failed };

/// How many statements one constant call may run, and how deep calls may nest, before it is
/// taken not to fold: a loop that never ends, or a recursion that never does.
constexpr std::uint64_t MaxSteps = 1'000'000;
constexpr int MaxDepth = 64;

/// How deep constant calls are nested now.
thread_local int depth = 0;

/// Runs the body of a function whose value folds: its statements over its parameters' and its
/// variables' values, held as a Folder's bindings, which is how its expressions fold too.
class ConstantRun {
public:
    explicit ConstantRun(Folder bound) : frame(std::move(bound)) {}

    /// Runs the statements of `container` in order.
    Flow statements(tree_t container) {
        for (unsigned index = 0; index < tree_stmts(container); ++index) {
            const Flow flow = statement(tree_stmt(container, index));
            if (flow != Flow::Continue) return flow;
        }
        return Flow::Continue;
    }

    /// Declares what a body declares: its variables, with their initial values. Constants and
    /// types fold where they are used, and anything else is not run.
    bool declare(tree_t container) {
        for (unsigned index = 0; index < tree_decls(container); ++index) {
            tree_t declaration = tree_decl(container, index);
            switch (tree_kind(declaration)) {
            case T_VAR_DECL: {
                type_t type = tree_type(declaration);
                if (type_is_array(type) || type_is_record(type)) {
                    // A composite variable is its bits, bound as an array generic's are.
                    const std::optional<Layout> layout = layoutOf(type, frame);
                    if (!layout) return false;
                    std::optional<eir::BitVector> initial =
                        tree_has_value(declaration)
                            ? constantBits(tree_value(declaration), *layout, frame)
                            : defaultBits(type, *layout, frame);
                    if (!initial) return false;
                    frame.bindComposite(declaration, std::move(*initial));
                    break;
                }
                if (!type_is_scalar(type)) return false;
                const std::optional<std::int64_t> initial =
                    tree_has_value(declaration) ? frame.integer(tree_value(declaration))
                                                : frame.left(type);
                if (!initial) return false;
                frame.bind(declaration, *initial);
                break;
            }

            // A file not opened where it is declared does nothing until something opens it, which
            // does not fold.
            case T_FILE_DECL:
                if (tree_has_value(declaration)) return false;
                break;
            case T_CONST_DECL:
            case T_TYPE_DECL:
            case T_SUBTYPE_DECL:
            case T_USE:
            case T_ATTR_SPEC:
            case T_FUNC_DECL:
            case T_FUNC_BODY: break;
            default: return false;
            }
        }
        return true;
    }

    /// The expression the `return` that ended the run names, and the bindings it folds in.
    [[nodiscard]] tree_t value() const noexcept { return result; }

    [[nodiscard]] const Folder& bindings() const noexcept { return frame; }

private:
    Flow statement(tree_t statement) {
        if (++steps > MaxSteps) return Flow::Failed;
        switch (tree_kind(statement)) {
        case T_NULL: return Flow::Continue;

        case T_VAR_ASSIGN: return assignment(tree_target(statement), tree_value(statement));

        case T_RETURN:
            if (!tree_has_value(statement)) return Flow::Failed;
            result = tree_value(statement);
            return Flow::Return;

        case T_IF:
            for (unsigned index = 0; index < tree_conds(statement); ++index) {
                tree_t alternative = tree_cond(statement, index);
                if (tree_has_value(alternative)) {
                    const std::optional<std::int64_t> holds =
                        frame.integer(tree_value(alternative));
                    if (!holds) return Flow::Failed;
                    if (*holds == 0) continue;
                }
                return statements(alternative);
            }
            return Flow::Continue;

        case T_CASE: return caseStatement(statement);

        case T_FOR: {
            const std::optional<Folder::Range> range = frame.range(tree_range(statement, 0));
            if (!range || tree_decls(statement) != 1) return Flow::Failed;
            for (std::int64_t offset = 0; offset < range->length(); ++offset) {
                frame.bind(tree_decl(statement, 0),
                           range->ascending ? range->left + offset : range->left - offset);
                const Flow flow = statements(statement);
                if (flow == Flow::Continue) continue;
                if (!leaves(flow, statement)) return flow;
                if (flow == Flow::Exit) break;
            }
            return Flow::Continue;
        }

        case T_WHILE:
            for (;;) {
                if (++steps > MaxSteps) return Flow::Failed;
                if (tree_has_value(statement)) {
                    const std::optional<std::int64_t> holds = frame.integer(tree_value(statement));
                    if (!holds) return Flow::Failed;
                    if (*holds == 0) return Flow::Continue;
                }
                const Flow flow = statements(statement);
                if (flow == Flow::Continue) continue;
                if (!leaves(flow, statement)) return flow;
                if (flow == Flow::Exit) return Flow::Continue;
            }

        case T_EXIT:
        case T_NEXT: {
            if (tree_has_value(statement)) {
                const std::optional<std::int64_t> holds = frame.integer(tree_value(statement));
                if (!holds) return Flow::Failed;
                if (*holds == 0) return Flow::Continue;
            }
            label = tree_has_ident2(statement) ? tree_ident2(statement) : nullptr;
            return tree_kind(statement) == T_EXIT ? Flow::Exit : Flow::Next;
        }

        // An assertion that holds does nothing; one that fails would report, which folding
        // cannot.
        case T_ASSERT: {
            const std::optional<std::int64_t> holds = frame.integer(tree_value(statement));
            return holds && *holds != 0 ? Flow::Continue : Flow::Failed;
        }

        default: return Flow::Failed;
        }
    }

    /// `target := value`, of a scalar variable, a composite one, or an element or a slice of an
    /// array one.
    Flow assignment(tree_t target, tree_t value) {
        const tree_kind_t targetKind = tree_kind(target);
        tree_t name =
            targetKind == T_ARRAY_REF || targetKind == T_ARRAY_SLICE ? tree_value(target) : target;
        if (tree_kind(name) != T_REF || !tree_has_ref(name)
            || tree_kind(tree_ref(name)) != T_VAR_DECL)
            return Flow::Failed;
        tree_t variable = tree_ref(name);
        type_t type = tree_type(variable);

        if (!type_is_array(type) && !type_is_record(type)) {
            const std::optional<std::int64_t> scalar = frame.integer(value);
            if (!scalar) return Flow::Failed;
            frame.bind(variable, *scalar);
            return Flow::Continue;
        }
        const std::optional<Layout> layout = layoutOf(type, frame);
        const eir::BitVector* current = frame.compositeOf(variable);
        if (!layout || current == nullptr) return Flow::Failed;

        if (target == name) {
            std::optional<eir::BitVector> bits = constantBits(value, *layout, frame);
            if (!bits) return Flow::Failed;
            frame.bindComposite(variable, std::move(*bits));
            return Flow::Continue;
        }

        // A slice, at the offset its right bound has in the whole, as wide as it is.
        if (targetKind == T_ARRAY_SLICE) {
            const std::optional<Folder::Range> slice = frame.range(tree_range(target, 0));
            if (!layout->isArray() || !layout->inner.empty() || !slice || slice->length() == 0
                || slice->ascending != layout->index.ascending || slice->low() < layout->index.low()
                || slice->high() > layout->index.high())
                return Flow::Failed;
            Layout part = *layout;
            part.index = *slice;
            part.storage = eir::Type::bits(
                static_cast<std::uint32_t>(slice->length()) * layout->elementBits, false, false);
            const std::optional<eir::BitVector> bits = constantBits(value, part, frame);
            if (!bits) return Flow::Failed;
            eir::BitVector updated = *current;
            eir::bitops::assignPartAt(updated.mutableRef(), bits->ref(),
                                      static_cast<std::uint64_t>(layout->bitOffset(slice->right)));
            frame.bindComposite(variable, std::move(updated));
            return Flow::Continue;
        }

        // One element, at the offset its index has in the whole.
        if (!layout->isArray() || !layout->inner.empty() || tree_params(target) != 1)
            return Flow::Failed;
        const std::optional<std::int64_t> index = frame.integer(tree_value(tree_param(target, 0)));
        const std::optional<Layout> element = elementLayout(*layout, frame);
        if (!index || !element || *index < layout->index.low() || *index > layout->index.high())
            return Flow::Failed;
        const std::optional<eir::BitVector> bits = constantBits(value, *element, frame);
        if (!bits) return Flow::Failed;
        eir::BitVector updated = *current;
        eir::bitops::assignPartAt(updated.mutableRef(), bits->ref(),
                                  static_cast<std::uint64_t>(layout->bitOffset(*index)));
        frame.bindComposite(variable, std::move(updated));
        return Flow::Continue;
    }

    Flow caseStatement(tree_t statement) {
        const std::optional<std::int64_t> selector = frame.integer(tree_value(statement));
        if (!selector) return Flow::Failed;
        for (unsigned index = 0; index < tree_stmts(statement); ++index) {
            tree_t alternative = tree_stmt(statement, index);
            for (unsigned choiceIndex = 0; choiceIndex < tree_choices(alternative); ++choiceIndex) {
                tree_t choice = tree_choice(alternative, choiceIndex);
                bool chosen = false;
                if (tree_has_name(choice)) {
                    const std::optional<std::int64_t> value = frame.integer(tree_name(choice));
                    if (!value) return Flow::Failed;
                    chosen = *value == *selector;
                } else if (tree_ranges(choice) > 0) {
                    const std::optional<Folder::Range> range = frame.range(tree_range(choice, 0));
                    if (!range) return Flow::Failed;
                    chosen = *selector >= range->low() && *selector <= range->high();
                } else {
                    chosen = true;
                }
                if (chosen) return statements(alternative);
            }
        }
        return Flow::Failed;
    }

    /// True when `flow`, an exit or a next, is one of `loop`'s: an unlabelled one, or one naming
    /// it. A labelled one naming a loop further out goes on out.
    bool leaves(Flow flow, tree_t loop) {
        if (flow != Flow::Exit && flow != Flow::Next) return false;
        if (label != nullptr && (!tree_has_ident(loop) || tree_ident(loop) != label)) return false;
        label = nullptr;
        return true;
    }

    Folder frame;
    tree_t result = nullptr;
    ident_t label = nullptr;
    std::uint64_t steps = 0;
};

/// Keeps count of how deep constant calls are nested while one runs.
class Nested {
public:
    Nested() noexcept { ++depth; }

    Nested(const Nested&) = delete;
    Nested& operator=(const Nested&) = delete;
    Nested(Nested&&) = delete;
    Nested& operator=(Nested&&) = delete;

    ~Nested() { --depth; }
};

} // namespace

std::optional<std::int64_t> Folder::constantCall(tree_t expression, tree_t declaration) const {
    type_t result = type_result(tree_type(declaration));
    if (!type_is_scalar(result)) return std::nullopt;
    std::optional<std::int64_t> value;
    returned(expression, [&value](tree_t returned, const Folder& frame) {
        value = frame.integer(returned);
        return value.has_value();
    });
    return value;
}

bool Folder::returned(tree_t call,
                      const std::function<bool(tree_t value, const Folder& frame)>& use) const {
    if (tree_kind(call) != T_FCALL || !tree_has_ref(call)) return false;
    tree_t declaration = tree_ref(call);
    const tree_kind_t kind = tree_kind(declaration);
    if (kind != T_FUNC_DECL && kind != T_FUNC_BODY) return false;
    if (!hasVhdlBody(static_cast<subprogram_kind_t>(tree_subkind(declaration)))) return false;
    // At elaboration an impure function folds too when what it runs is static: one that would read
    // a file only when given a name, and is given none. Opening the file does not fold.
    if (((tree_flags(declaration) & TREE_F_IMPURE) != 0 && !impureAllowed) || depth >= MaxDepth)
        return false;
    tree_t body = subprogramBody(declaration);
    if (body == nullptr || tree_ports(body) != tree_ports(declaration)) return false;

    // The body sees what the caller sees, an architecture's generics among it, and its own
    // parameters, each its actual's value or its default. The declaration's parameters are what
    // a named association names, and the body's are in the same order.
    Folder frame = *this;
    for (unsigned index = 0; index < tree_ports(declaration); ++index) {
        tree_t formal = tree_port(declaration, index);
        tree_t actual = tree_has_value(formal) ? tree_value(formal) : nullptr;
        for (unsigned param = 0; param < tree_params(call); ++param) {
            tree_t association = tree_param(call, param);
            const bool positional =
                tree_subkind(association) == P_POS && tree_pos(association) == index;
            const bool named = tree_subkind(association) == P_NAMED
                               && tree_kind(tree_name(association)) == T_REF
                               && tree_has_ref(tree_name(association))
                               && tree_ref(tree_name(association)) == formal;
            if ((positional || named) && tree_kind(tree_value(association)) != T_OPEN)
                actual = tree_value(association);
        }
        if (actual == nullptr) return false;

        tree_t parameter = tree_port(body, index);
        type_t type = tree_type(parameter);
        if (type_is_scalar(type)) {
            const std::optional<std::int64_t> value = integer(actual);
            if (!value) return false;
            frame.bind(parameter, *value);
        } else if (type_is_array(type)) {
            const std::optional<Range> bounds = boundsOf(actual);
            if (!bounds) return false;
            frame.bindBounds(parameter, *bounds);
            const std::optional<Layout> layout =
                bounds->length() == 0 ? std::nullopt : arrayLayout(*bounds, type_elem(type), *this);
            if (layout) {
                std::optional<eir::BitVector> bits = constantBits(actual, *layout, *this);
                if (!bits) return false;
                frame.bindComposite(parameter, std::move(*bits));
            }
        } else {
            return false;
        }
    }

    const Nested nested;
    ConstantRun run(std::move(frame));
    if (!run.declare(body) || run.statements(body) != Flow::Return) return false;
    return use(run.value(), run.bindings());
}

std::optional<std::int64_t> Folder::applyToArray(tree_t declaration, const eir::BitVector& elements,
                                                 const Range& bounds) const {
    tree_t body = subprogramBody(declaration);
    if (body == nullptr || tree_kind(body) != T_FUNC_BODY || tree_ports(body) != 1
        || depth >= MaxDepth)
        return std::nullopt;
    Folder frame = *this;
    tree_t parameter = tree_port(body, 0);
    frame.bindBounds(parameter, bounds);
    frame.bindComposite(parameter, elements);

    const Nested nested;
    ConstantRun run(std::move(frame));
    if (!run.declare(body) || run.statements(body) != Flow::Return) return std::nullopt;
    return run.bindings().integer(run.value());
}

std::optional<std::int64_t> Folder::element(tree_t expression) const {
    tree_t prefix = tree_value(expression);
    if (tree_kind(prefix) != T_REF || !tree_has_ref(prefix) || tree_params(expression) != 1)
        return std::nullopt;
    // An array bound here, or a constant's value, the array of a generate statement's conditions
    // say.
    tree_t declaration = tree_ref(prefix);
    const eir::BitVector* bits = compositeOf(declaration);
    const std::optional<Range> folded = boundsOf(prefix);
    std::optional<eir::BitVector> value;
    tree_t constant = constantValue(declaration);
    if (bits == nullptr && folded && folded->length() > 0 && constant != nullptr) {
        type_t type = tree_type(declaration);
        if (const std::optional<Layout> layout = arrayLayout(*folded, type_elem(type), *this))
            value = constantBits(constant, *layout, *this);
        if (value) bits = &*value;
    }
    const std::optional<std::int64_t> index = integer(tree_value(tree_param(expression, 0)));
    if (bits == nullptr || !folded || !index) return std::nullopt;
    const Range bounds = *folded;
    const std::int64_t position = *index;
    if (position < bounds.low() || position > bounds.high()) return std::nullopt;

    // The left element is in the highest bits (DESIGN_GUIDE.md section 3.6).
    const auto length = static_cast<std::uint64_t>(bounds.length());
    if (length == 0 || bits->width() % length != 0) return std::nullopt;
    const std::uint64_t elementBits = bits->width() / length;
    if (elementBits == 0 || elementBits > 64) return std::nullopt;
    const auto fromRight = static_cast<std::uint64_t>(bounds.ascending ? bounds.right - position
                                                                       : position - bounds.right);
    eir::BitVector one(static_cast<std::uint32_t>(elementBits));
    eir::bitops::partSelectAt(one.mutableRef(), bits->ref(),
                              static_cast<std::uint32_t>(fromRight * elementBits));
    const std::uint64_t raw = one.ref().toUInt64();
    if (!isSignedScalar(tree_type(expression)) || elementBits == 64)
        return static_cast<std::int64_t>(raw);
    const std::uint64_t sign = std::uint64_t{1} << (elementBits - 1);
    return static_cast<std::int64_t>((raw ^ sign) - sign);
}

} // namespace enzan::frontend::vhdl
