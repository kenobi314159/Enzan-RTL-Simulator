// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace enzan {

/// Returns the directory holding the running executable, or an empty path when `/proc/self/exe`
/// cannot be read.
///
/// The rest of Enzan is found from here rather than from the prefix it was configured with, so
/// that an installation can be moved, or kept beside another version of itself, and a build tree
/// runs its own parts rather than an installed copy's (DESIGN_GUIDE.md section 11.5).
std::filesystem::path programDirectory();

/// Returns the graphical front end of the Enzan whose executables are in `programDirectory`, or an
/// empty path when that is empty.
///
/// It is in `libexec` rather than on the `PATH`, because it is started only by `enzan` (section
/// 8.4), and it is never looked for anywhere else: a front end found on the `PATH` could be
/// another version's.
std::filesystem::path frontEndProgram(const std::filesystem::path& programDirectory);

/// Returns where VHDL's standard libraries are for the Enzan whose executables are in
/// `programDirectory`, or an empty path when that is empty (section 4.4).
std::filesystem::path vhdlLibraryDirectory(const std::filesystem::path& programDirectory);

} // namespace enzan
