// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/Installation.h"

#include "common/InstallationInfo.h"

#include <system_error>

namespace enzan {

namespace {

/// `relative` followed from `directory`, its `..` resolved so that a message names the place.
std::filesystem::path from(const std::filesystem::path& directory, const char* relative) {
    if (directory.empty()) return {};
    return (directory / relative).lexically_normal();
}

} // namespace

std::filesystem::path programDirectory() {
    std::error_code failure;
    const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure) return {};
    return program.parent_path();
}

std::filesystem::path frontEndProgram(const std::filesystem::path& programDirectory) {
    return from(programDirectory, ENZAN_FRONT_END_FROM_PROGRAM);
}

std::filesystem::path vhdlLibraryDirectory(const std::filesystem::path& programDirectory) {
    return from(programDirectory, ENZAN_VHDL_LIBRARIES_FROM_PROGRAM);
}

} // namespace enzan
