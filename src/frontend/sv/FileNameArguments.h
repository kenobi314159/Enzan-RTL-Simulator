// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <set>
#include <span>

#include "slang/text/SourceLocation.h"

namespace slang {
class Diagnostic;
} // namespace slang

namespace slang::syntax {
class SyntaxTree;
} // namespace slang::syntax

namespace enzan::frontend::sv {

/// The file names and modes `$fopen` is given as integral values holding their characters, for
/// the errors that are excused in them.
///
/// IEEE 1800 section 21.3.1 allows a file name and a mode to be a string literal, a `string` or
/// an integral value whose bytes hold the characters, which is how Verilog holds a name: in a
/// `reg [8*256:1]`, as Altera's memory-file readers do. slang types both arguments `string`,
/// which an integral value does not convert to without a cast, and refuses the third form. The
/// translation of `$fopen` reads such a value as the characters it holds.
///
/// The arguments are found from the sources' syntax rather than the elaborated design, since
/// slang checks every module, those no instance names included.
class FileNameArguments {
public:
    /// Records where the `$fopen` calls of `trees` have their arguments.
    explicit FileNameArguments(std::span<const std::shared_ptr<slang::syntax::SyntaxTree>> trees);

    /// True when `diagnostic` is slang's refusal of an integral value as a `$fopen` argument.
    [[nodiscard]] bool excuses(const slang::Diagnostic& diagnostic) const;

private:
    std::set<slang::SourceLocation> arguments;
};

} // namespace enzan::frontend::sv
