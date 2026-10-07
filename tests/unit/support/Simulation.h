// SPDX-License-Identifier: GPL-3.0-or-later
//
// A one-module design taken from hand-built EIR to a kernel at time zero.
//
// The tests that drive the kernel build EIR by hand rather than parsing SystemVerilog, so that
// they test the kernel without depending on the frontend and a scheduling bug cannot be
// confused with a frontend one. Every such test needs the same pipeline — verify, elaborate,
// compile, build an image, start a kernel — and this is it, written once.

#ifndef ENZAN_TESTS_UNIT_SUPPORT_SIMULATION_H
#define ENZAN_TESTS_UNIT_SUPPORT_SIMULATION_H

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Verifier.h"
#include "elab/ElaboratedDesign.h"
#include "elab/Elaborator.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enzan::test {

class CollectingConsumer final : public DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override {
        messages.push_back(diagnostic.message);
        codes.push_back(diagnostic.code);
    }

    std::vector<std::string> messages;
    std::vector<DiagCode> codes;
};

/// Verifies, elaborates, compiles and runs a one-module design.
class Simulation {
public:
    /// The buffer `Simulation` registers its stand-in source text as.
    ///
    /// Always the first buffer of the manager, so that a test can build EIR carrying
    /// locations into it *before* the simulation that owns the manager exists. Identifiers
    /// are handed out in the order buffers are added, which is what makes that sound.
    static constexpr BufferId SourceBuffer = BufferId::fromIndex(0);

    /// One line of stand-in source per line number, so that a location is easy to write.
    static constexpr std::string_view SourceText = "one\ntwo\nthree\nfour\nfive\n";

    /// The byte offset of 1-based `line` in `SourceText`.
    static constexpr std::uint32_t lineOffset(std::uint32_t line) {
        constexpr std::uint32_t Offsets[] = {0, 0, 4, 8, 14, 19};
        return Offsets[line];
    }

    /// A location on `line` of the stand-in source.
    static constexpr SourceLocation at(std::uint32_t line) {
        return {SourceBuffer, lineOffset(line)};
    }

    /// A location `column` bytes into `line`, for a test that needs two positions on one line.
    ///
    /// Every line of the stand-in source is at least three bytes long, so a small column stays
    /// on the line it was asked for.
    static constexpr SourceLocation at(std::uint32_t line, std::uint32_t column) {
        return {SourceBuffer, lineOffset(line) + column};
    }

    Simulation(eir::Design design, std::string_view topName)
        : engine(sources), analysed(std::move(design)) {
        // Added first, so that `SourceBuffer` names it. Every simulation gets one even when
        // the test does not care, because a fixture whose buffer numbering depends on what
        // the test asked for is a fixture that breaks the next test to be written.
        const BufferId registered = sources.addBuffer("design.sv", std::string(SourceText));
        EXPECT_EQ(registered, SourceBuffer);

        engine.addConsumer(consumer);

        // Verifying here means every kernel test also exercises the verifier, and that a
        // test whose hand-built EIR is malformed says so instead of misbehaving.
        valid = eir::verify(analysed, engine);

        elab::Elaborator elaborator(analysed, engine);
        elaborated = elaborator.elaborate(topName);
        if (elaborated == nullptr) return;

        codegen::Compiler compiler(engine);
        compiled = compiler.compile(elaborated->units());
        if (compiled == nullptr) return;

        image = std::make_unique<runtime::Image>(*elaborated, *compiled);

        runtime::KernelOptions options;
        options.transcript = &transcriptStream;
        options.deltaLimit = deltaLimit;
        kernel = std::make_unique<runtime::Kernel>(*image, engine, options);
        kernel->initialize();
    }

    [[nodiscard]] bool ready() const noexcept { return kernel != nullptr && valid; }

    runtime::RunStatus run(runtime::SimTime until = runtime::NoTime) { return kernel->run(until); }

    [[nodiscard]] runtime::SimTime time() const { return kernel->time(); }

    /// Renders a signal's current value, so that assertions read as the design does.
    [[nodiscard]] std::string value(std::string_view path,
                                    eir::Radix radix = eir::Radix::Binary) const {
        const std::uint32_t signalIndex = image->findSignal(path);
        EXPECT_NE(signalIndex, runtime::Image::NoSignal) << "no signal named " << path;
        if (signalIndex == runtime::Image::NoSignal) return "<missing>";

        const auto& signal = image->signals()[signalIndex];
        return eir::toString(kernel->arena().at(signal.wordOffset, signal.bitWidth), radix);
    }

    /// The index of a signal by path, failing the test if the design has no such signal.
    [[nodiscard]] std::uint32_t signal(std::string_view path) const {
        const std::uint32_t signalIndex = image->findSignal(path);
        EXPECT_NE(signalIndex, runtime::Image::NoSignal) << "no signal named " << path;
        return signalIndex;
    }

    [[nodiscard]] std::string transcript() const { return transcriptStream.str(); }

    [[nodiscard]] bool reportedCode(DiagCode code) const {
        return std::ranges::find(consumer.codes, code) != consumer.codes.end();
    }

    [[nodiscard]] const std::vector<std::string>& messages() const { return consumer.messages; }

    runtime::Kernel& simulationKernel() { return *kernel; }

    [[nodiscard]] const runtime::Image& designImage() const { return *image; }

    /// Delta limit applied to the kernel. Set before constructing to test the limit
    /// without waiting for ten thousand iterations.
    inline static std::uint32_t deltaLimit = 10'000;

private:
    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    std::ostringstream transcriptStream;

    eir::Design analysed;
    std::unique_ptr<elab::ElaboratedDesign> elaborated;
    std::unique_ptr<codegen::CompiledDesign> compiled;
    std::unique_ptr<runtime::Image> image;
    std::unique_ptr<runtime::Kernel> kernel;
    bool valid = false;
};

} // namespace enzan::test

#endif // ENZAN_TESTS_UNIT_SUPPORT_SIMULATION_H
