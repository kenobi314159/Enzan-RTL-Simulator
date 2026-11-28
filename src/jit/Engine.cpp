// SPDX-License-Identifier: GPL-3.0-or-later

#include "jit/Engine.h"

#include "common/Diagnostics.h"
#include "runtime/ExecutionHelpers.h"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <mutex>
#include <string>
#include <utility>

namespace enzan::jit {

namespace {

void report(DiagnosticEngine& diagnostics, llvm::Error error) {
    diagnostics.report(DiagCode::JitPreparationFailed, {}, llvm::toString(std::move(error)));
}

} // namespace

Engine::Engine(Key /*key*/, DiagnosticEngine& diagnosticEngine,
               std::unique_ptr<llvm::orc::LLJIT> instance)
    : diagnostics(diagnosticEngine), jit(std::move(instance)) {}

Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::create(DiagnosticEngine& diagnostics) {
    static std::once_flag once;
    static bool targetFailed = false;
    std::call_once(once, [] {
        targetFailed = llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter();
    });
    if (targetFailed) {
        diagnostics.report(DiagCode::JitPreparationFailed, {}, "host target initialization failed");
        return nullptr;
    }

    auto built =
        llvm::orc::LLJITBuilder().setPlatformSetUp(llvm::orc::setUpInactivePlatform).create();
    if (!built) {
        report(diagnostics, built.takeError());
        return nullptr;
    }
    auto instance = std::make_unique<Engine>(Key{}, diagnostics, std::move(*built));
    instance->jit->getExecutionSession().setErrorReporter(
        [&diagnostics](llvm::Error error) { report(diagnostics, std::move(error)); });

    llvm::orc::MangleAndInterner mangle(instance->jit->getExecutionSession(),
                                        instance->jit->getDataLayout());
    llvm::orc::SymbolMap symbols;
    const auto add = [&](const char* name, auto* address) {
        symbols[mangle(name)] = {llvm::orc::ExecutorAddr::fromPtr(address),
                                 llvm::JITSymbolFlags::Exported};
    };
    add("enzanExecResumePc", &runtime::enzanExecResumePc);
    add("enzanExecFrame", &runtime::enzanExecFrame);
    add("enzanExecFrameAndCounter", &runtime::enzanExecFrameAndCounter);
    add("enzanExecRequestCall", &runtime::enzanExecRequestCall);
    add("enzanExecExtend", &runtime::enzanExecExtend);
    add("enzanExecPresent", &runtime::enzanExecPresent);
    add("enzanExecEncode", &runtime::enzanExecEncode);
    add("enzanExecBitSelect", &runtime::enzanExecBitSelect);
    add("enzanExecPartSelect", &runtime::enzanExecPartSelect);
    add("enzanExecSplicePart", &runtime::enzanExecSplicePart);
    add("enzanExecCompare", &runtime::enzanExecCompare);
    add("enzanExecDivRem", &runtime::enzanExecDivRem);
    add("enzanExecWideArithmetic", &runtime::enzanExecWideArithmetic);
    add("enzanExecWideShift", &runtime::enzanExecWideShift);
    add("enzanExecReadObject", &runtime::enzanExecReadObject);
    add("enzanExecReadObjectPart", &runtime::enzanExecReadObjectPart);
    add("enzanExecSignalAttribute", &runtime::enzanExecSignalAttribute);
    add("enzanExecLastValue", &runtime::enzanExecLastValue);
    add("enzanExecTime", &runtime::enzanExecTime);
    add("enzanExecStore", &runtime::enzanExecStore);
    add("enzanExecStoreNonBlocking", &runtime::enzanExecStoreNonBlocking);
    add("enzanExecStorePart", &runtime::enzanExecStorePart);
    add("enzanExecDrive", &runtime::enzanExecDrive);
    add("enzanExecDrivePart", &runtime::enzanExecDrivePart);
    add("enzanExecWaitTime", &runtime::enzanExecWaitTime);
    add("enzanExecWaitEdge", &runtime::enzanExecWaitEdge);
    add("enzanExecWaitEvents", &runtime::enzanExecWaitEvents);
    add("enzanExecSystemTask", &runtime::enzanExecSystemTask);
    add("enzanExecSystemFunction", &runtime::enzanExecSystemFunction);
    add("enzanExecCount", &runtime::enzanExecCount);
    if (auto error = instance->jit->getMainJITDylib().define(
            llvm::orc::absoluteSymbols(std::move(symbols)))) {
        report(diagnostics, std::move(error));
        return nullptr;
    }
    return instance;
}

bool Engine::addModule(llvm::orc::ThreadSafeModule module) {
    std::string reason;
    const bool invalid = module.withModuleDo([&](llvm::Module& body) {
        body.setDataLayout(jit->getDataLayout());
        body.setTargetTriple(jit->getTargetTriple().str());
        llvm::raw_string_ostream output(reason);
        return llvm::verifyModule(body, &output);
    });
    if (invalid) {
        diagnostics.report(DiagCode::JitPreparationFailed, {}, reason);
        return false;
    }
    if (auto error = jit->addIRModule(std::move(module))) {
        report(diagnostics, std::move(error));
        return false;
    }
    return true;
}

NativeBody Engine::lookup(std::string_view name) {
    auto symbol = jit->lookup(llvm::StringRef(name.data(), name.size()));
    if (!symbol) {
        report(diagnostics, symbol.takeError());
        return nullptr;
    }
    return symbol->toPtr<NativeBody>();
}

} // namespace enzan::jit
