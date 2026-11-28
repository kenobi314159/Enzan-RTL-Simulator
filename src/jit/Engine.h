// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace llvm::orc {
class LLJIT;
class ThreadSafeModule;
} // namespace llvm::orc

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::runtime {
class ExecutionContext;
}

namespace enzan::jit {

/// Calling convention for a generated process or subprogram body.
using NativeBody = std::uint32_t (*)(runtime::ExecutionContext*);

/// Owns the native code for one prepared simulation.
/// The diagnostic engine must outlive this object and its generated entry points.
class Engine {
    struct Key {};

public:
    /// Used by `create` to put the engine directly under unique ownership.
    Engine(Key /*key*/, DiagnosticEngine& diagnosticEngine,
           std::unique_ptr<llvm::orc::LLJIT> instance);

    /// Initializes the host target and registers the runtime helper symbols.
    /// Returns null after reporting a diagnostic if preparation fails.
    static std::unique_ptr<Engine> create(DiagnosticEngine& diagnostics);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    /// Verifies and adds a module for the host. Failure reports a diagnostic.
    bool addModule(llvm::orc::ThreadSafeModule module);

    /// Resolves a generated entry point, or returns null after a diagnostic.
    /// The resulting address is valid only while this engine is alive.
    NativeBody lookup(std::string_view name);

private:
    DiagnosticEngine& diagnostics;
    std::unique_ptr<llvm::orc::LLJIT> jit;
};

} // namespace enzan::jit
