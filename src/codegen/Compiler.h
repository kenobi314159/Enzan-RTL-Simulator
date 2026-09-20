// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "codegen/Bytecode.h"
#include "eir/Design.h"

#include <memory>
#include <string>
#include <vector>

namespace enzan {
class DiagnosticEngine;
}

namespace enzan::codegen {

/// Lowers EIR-B to the bytecode the virtual machine executes.
///
/// The bytecode is register-based rather than stack-based, as DESIGN_GUIDE.md section
/// 6.1 requires: it produces fewer instructions per operation and maps far more
/// directly onto both real registers and LLVM IR.
///
/// The lowering is deliberately simple and does no optimisation at all — no constant
/// folding, no dead code elimination, no register coalescing. Optimisation belongs in
/// elaboration (section 2.1) or in the JIT (section 6.3), and an optimising bytecode
/// compiler would be a third place for a semantic bug to hide while the interpreter is
/// still the reference the JIT will be differentially tested against.
///
/// What it does do is resolve: virtual registers become Word offsets into a frame,
/// blocks become instruction indices, constants are flattened into one array, and
/// every width the operation needs is written into the instruction. After this, the
/// interpreter's inner loop needs nothing but the instruction and the data.
class Compiler {
public:
    /// `diagnostics` must outlive the compiler.
    explicit Compiler(DiagnosticEngine& engine) noexcept;

    /// Compiles every process of every definition in `design`.
    ///
    /// Compiling the whole design rather than only what the current top instantiates:
    /// the result is keyed by definition, so it is reusable across top levels, and
    /// compiling a body nobody instantiates costs a few microseconds and removes a
    /// reason for the runtime to call back into codegen.
    ///
    /// Returns nullptr having reported a diagnostic if any body cannot be compiled.
    std::unique_ptr<CompiledDesign> compile(const eir::Design& design);

private:
    /// Compiles one process body. Returns false having reported why, on failure.
    bool compileProcess(const eir::ModuleDefinition& definition, const eir::Process& process,
                        CompiledProcess& out);

    /// Resolves the process's system task descriptors against the register frame and
    /// the compiled constant pool, which must already be built.
    bool compileSysTaskCalls(const eir::ModuleDefinition& definition, const eir::Process& process,
                             const std::vector<std::uint32_t>& registerOffsets,
                             const std::vector<std::uint32_t>& registerWidths,
                             CompiledProcess& out);

    /// Reports malformed EIR, naming the module and the process.
    ///
    /// Reaching any of these means the EIR verifier was not run, so the message says
    /// so: a compiler that quietly tolerates bad input is how a frontend bug becomes a
    /// runtime crash.
    void reportInvalid(const eir::ModuleDefinition& definition, const eir::Process& process,
                       const std::string& message);

    DiagnosticEngine& diagnostics;
};

} // namespace enzan::codegen
