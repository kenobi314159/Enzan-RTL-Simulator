// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/Session.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Instruction.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"
#include "runtime/Kernel.h"
#include "support/Literals.h"
#include "support/ProcessBuilder.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace {

using namespace enzan;

std::unique_ptr<eir::Design> design(bool unsupported = false) {
    eir::ModuleDefinition top;
    top.name = "tb";
    top.addObject(eir::Object{.name = "source",
                              .type = eir::Type::bits(4, false, true),
                              .initialValue = test::literal("4'b1xz0")});
    top.addObject(eir::Object{.name = "output", .type = eir::Type::bits(4, false, true)});
    test::ProcessBuilder process(eir::ProcessKind::Initial, "driver");
    const auto value = process.reg(eir::Type::bits(4, false, true));
    const auto quotient = process.reg(eir::Type::bits(4, false, true));
    const auto delay = process.reg(eir::Type::bits(32, false, true));
    const auto constant = process.constant("32'd1");
    process.block("entry");
    process.emit(eir::Instruction::make(eir::Opcode::LoadConstant, eir::Operand::reg(delay),
                                        eir::Operand::constant(constant)));
    process.readObject(value, 0);
    process.store(1, value);
    if (unsupported) {
        process.emit(eir::Instruction::make(eir::Opcode::Divide, eir::Operand::reg(quotient),
                                            eir::Operand::reg(value), eir::Operand::reg(value)));
    }
    process.emit(eir::Instruction::make(eir::Opcode::WaitTime, {}, eir::Operand::reg(delay),
                                        eir::Operand::block(1)));
    process.block("resume");
    process.emit(eir::Instruction::make(eir::Opcode::Return));
    top.processes.push_back(process.finish());
    auto result = std::make_unique<eir::Design>();
    result->addModule(std::move(top));
    return result;
}

TEST(JitSession, NativeProcessResumesAndRestarts) {
    SourceManager sources;
    DiagnosticEngine diagnostics(sources);
    session::Session session(sources, diagnostics);
    session.setJitEnabled(true);
    session.enablePhaseTimings();
    session.setDesign(design());
    ASSERT_TRUE(session.elaborate("tb"));
    const auto output = session.findSignal("/tb/output");
    ASSERT_NE(output, session::Session::NoSignal);
    EXPECT_EQ(session.run(0), runtime::RunStatus::TimeLimitReached);
    EXPECT_EQ(session.examine(output, eir::Radix::Binary), "1xz0");
    EXPECT_EQ(session.run(std::nullopt), runtime::RunStatus::Quiescent);
    EXPECT_GT(session.phaseTimings().jit.count(), 0);
    ASSERT_TRUE(session.restart());
    EXPECT_EQ(session.run(std::nullopt), runtime::RunStatus::Quiescent);
    EXPECT_EQ(session.examine(output, eir::Radix::Binary), "1xz0");
}

TEST(JitSession, FailureLeavesNoKernelAndCanRetryWithInterpreter) {
    SourceManager sources;
    DiagnosticEngine diagnostics(sources);
    session::Session session(sources, diagnostics);
    session.setJitEnabled(true);
    session.setDesign(design(true));
    EXPECT_FALSE(session.elaborate("tb"));
    EXPECT_EQ(session.simulationKernel(), nullptr);
    EXPECT_FALSE(session.isElaborated());
    session.setJitEnabled(false);
    ASSERT_TRUE(session.elaborate("tb"));
    EXPECT_EQ(session.run(std::nullopt), runtime::RunStatus::Quiescent);
}

} // namespace
