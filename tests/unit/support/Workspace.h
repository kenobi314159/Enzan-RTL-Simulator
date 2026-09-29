// SPDX-License-Identifier: GPL-3.0-or-later
//
// A scratch directory a test writes files into, and the working directory a test stands in.
//
// Several fixtures need real files on disk — file lists name paths, a pre-scan reads sources,
// a library is a directory — and each had grown its own temporary-directory dance. This is
// that dance, written once, with the cleanup in a destructor so that a failing assertion does
// not leave a directory behind.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_WORKSPACE_H
#define ENZAN_TESTS_UNIT_SUPPORT_WORKSPACE_H

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace enzan::test {

/// A uniquely named directory under the system temporary directory, removed on destruction.
class Workspace {
public:
    Workspace() {
        static std::atomic<unsigned> counter{0};
        root = std::filesystem::temp_directory_path()
               / ("enzan-test-" + std::to_string(::getpid()) + "-"
                  + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(root);
    }

    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    ~Workspace() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    /// Writes `content` to `name`, creating any directories the name implies.
    void write(std::string_view name, std::string_view content) const {
        const std::filesystem::path target = root / name;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target);
        file << content;
    }

    /// Returns `name` resolved inside the workspace, whether or not it exists.
    [[nodiscard]] std::filesystem::path operator/(std::string_view name) const {
        return root / name;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root; }

private:
    std::filesystem::path root;
};

/// Stands the process in `directory` for as long as this object lives.
///
/// Needed where the behaviour under test is defined in terms of the working directory — a
/// `.f` file's relative paths are (DESIGN_GUIDE.md section 8.3) — because testing that
/// convention from the wrong directory tests something else.
class ScopedWorkingDirectory {
public:
    explicit ScopedWorkingDirectory(const std::filesystem::path& directory)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(directory);
    }

    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory(ScopedWorkingDirectory&&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory&&) = delete;

    ~ScopedWorkingDirectory() {
        std::error_code ignored;
        std::filesystem::current_path(previous, ignored);
    }

private:
    std::filesystem::path previous;
};

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_WORKSPACE_H
