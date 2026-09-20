// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace enzan {

/// Returns the simulator version, including any pre-release suffix.
///
/// The form is `MAJOR.MINOR.PATCH` optionally followed by `-SUFFIX`, for
/// example `0.1.0-dev`. The value is fixed at configure time and is stable for
/// the lifetime of the process, so callers may hold the returned view.
std::string_view version() noexcept;

/// Returns the abbreviated source revision the binary was built from, or
/// `"unknown"` when the build tree was not a git checkout.
///
/// This is the revision as of the last CMake *configure*, not the current state
/// of the working tree: building after a commit without reconfiguring leaves it
/// stale. It exists so that a bug report identifies a revision, so an
/// approximate answer is far better than none.
std::string_view sourceRevision() noexcept;

/// Returns the one-line identification printed first by `enzan --version`.
///
/// The form is `enzan <version> (<revision>)`. This line is parsed by scripts
/// and by the GUI's about box, so its shape must not change casually; the
/// human-readable detail belongs in the lines that follow it.
std::string versionLine();

/// Returns a multi-line description of the build: compiler, build type and
/// enabled optional components.
///
/// Intended for `enzan --version` and for inclusion in bug reports, where
/// knowing whether sanitizers or the JIT were active usually explains the
/// behaviour being reported.
std::string buildDescription();

} // namespace enzan
