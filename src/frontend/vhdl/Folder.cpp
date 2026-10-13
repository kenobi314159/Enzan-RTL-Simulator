// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend/vhdl/Folder.h"

#include <string>
#include <vector>

namespace enzan::frontend::vhdl {

namespace {

/// VHDL's `mod`, whose result takes the sign of its right operand (IEEE 1076 section 9.2.7),
/// where C++'s `%` is VHDL's `rem` and takes the sign of its left one.
std::int64_t modulo(std::int64_t left, std::int64_t right) {
    const std::int64_t remainder = left % right;
    return remainder != 0 && ((remainder < 0) != (right < 0)) ? remainder + right : remainder;
}

/// The value of a deferred constant whose full declaration computes it with one of NVC's foreign
/// functions, which Enzan supplies (DESIGN_GUIDE.md section 5.2), or nothing for any other.
std::optional<std::int64_t> foreignConstant(tree_t declaration) {
    tree_t unit = tree_container(declaration);
    if (tree_kind(unit) != T_PACKAGE) return std::nullopt;
    const std::string name =
        std::string(istr(tree_ident(unit))) + "." + istr(tree_ident(declaration));

    // Whether numeric_std's warnings of metavalues and truncation are silenced: they are not, as
    // they are not by default in GHDL or in the IEEE packages themselves.
    if (name == "NVC.SIM_PKG.IEEE_NO_WARNING") return 0;
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

std::optional<std::int64_t> Folder::integer(tree_t expression) const {
    switch (tree_kind(expression)) {
    case T_LITERAL:
        switch (tree_subkind(expression)) {
        case L_INT: return tree_ival(expression);
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

    // Between discrete and physical types a conversion keeps the value; from a real it rounds,
    // which this does not fold.
    case T_TYPE_CONV:
        if (type_is_real(tree_type(tree_value(expression)))) return std::nullopt;
        return integer(tree_value(expression));

    case T_REF: return reference(expression);
    case T_FCALL: return call(expression);
    case T_ATTR_REF: return attribute(expression);
    default: return std::nullopt;
    }
}

std::optional<std::int64_t> Folder::reference(tree_t expression) const {
    if (!tree_has_ref(expression)) return std::nullopt;
    tree_t declaration = tree_ref(expression);
    switch (tree_kind(declaration)) {
    case T_ENUM_LIT: return tree_pos(declaration);

    // A deferred constant has its value in the package body, which a design need not have
    // analysed; that one is not folded.
    case T_CONST_DECL:
        if (!tree_has_value(declaration)) return foreignConstant(declaration);
        return integer(tree_value(declaration));

    case T_GENERIC_DECL:
    case T_PARAM_DECL: {
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
    if (kind == S_USER) return std::nullopt;

    // Two arrays of different lengths are never equal, and two null arrays always are, whatever
    // their elements would be: the bounds decide, without the values.
    if ((kind == S_ARRAY_EQ || kind == S_ARRAY_NEQ) && tree_params(expression) == 2) {
        const std::optional<Range> left = boundsOf(tree_value(tree_param(expression, 0)));
        const std::optional<Range> right = boundsOf(tree_value(tree_param(expression, 1)));
        if (!left || !right) return std::nullopt;
        if (left->length() != right->length()) return kind == S_ARRAY_NEQ ? 1 : 0;
        if (left->length() == 0) return kind == S_ARRAY_EQ ? 1 : 0;
        return std::nullopt;
    }

    std::vector<std::int64_t> operands;
    const unsigned count = tree_params(expression);
    for (unsigned index = 0; index < count; ++index) {
        const std::optional<std::int64_t> operand =
            integer(tree_value(tree_param(expression, index)));
        if (!operand) return std::nullopt;
        operands.push_back(*operand);
    }

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
    case ATTR_LENGTH: {
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
    return tree_has_type(name) ? rangeOf(tree_type(name), dimension) : std::nullopt;
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

} // namespace enzan::frontend::vhdl
