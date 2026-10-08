// SPDX-License-Identifier: GPL-3.0-or-later

#include "session/ConstantFunctionRunner.h"

#include "codegen/Compiler.h"
#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/BitOps.h"
#include "elab/ElaboratedDesign.h"
#include "elab/Elaborator.h"
#include "runtime/Image.h"
#include "runtime/Kernel.h"

#include <format>
#include <memory>
#include <sstream>

namespace enzan::session {

ConstantFunctionRunner::ConstantFunctionRunner(const eir::Design& analysed,
                                               const SourceManager& sourceManager,
                                               DiagnosticEngine& engine) noexcept
    : design(&analysed), sources(&sourceManager), diagnostics(&engine) {}

std::optional<eir::BitVector>
ConstantFunctionRunner::call(std::string_view function,
                             const std::optional<eir::BitVector>& arguments, std::uint32_t width) {
    const eir::ModuleDefinition* unit = design->findModule(function);
    if (unit == nullptr || !unit->isFunction || unit->ports.empty()) return std::nullopt;

    const std::string key =
        arguments.has_value() ? eir::toString(arguments->ref(), eir::Radix::Binary) : "";
    const auto known = results.find(std::pair(std::string(function), key));
    if (known != results.end()) return known->second.resized(width, false);

    if (!trusted(*unit)) return std::nullopt;

    // The arguments, first highest, become the inputs' initial values, as slang's call did.
    eir::ModuleDefinition called = *unit;
    std::uint32_t argumentBits = 0;
    for (std::size_t index = 0; index + 1 < called.ports.size(); ++index)
        argumentBits += called.objects[called.ports[index].objectIndex].type.bitWidth();
    if (argumentBits != (arguments.has_value() ? arguments->width() : 0)) return std::nullopt;

    std::uint32_t low = argumentBits;
    for (std::size_t index = 0; index + 1 < called.ports.size(); ++index) {
        eir::Object& input = called.objects[called.ports[index].objectIndex];
        const std::uint32_t inputWidth = input.type.bitWidth();
        low -= inputWidth;
        eir::BitVector value(inputWidth);
        eir::bitops::partSelectAt(value.mutableRef(), arguments->ref(), low);
        input.initialValue = std::move(value);
    }

    const std::optional<eir::BitVector> returned = run(called);
    if (!returned.has_value()) return std::nullopt;
    results.emplace(std::pair(std::string(function), key), *returned);
    return returned->resized(width, false);
}

bool ConstantFunctionRunner::trusted(const eir::ModuleDefinition& unit) {
    const auto found = checked.find(unit.name);
    if (found != checked.end()) return found->second;

    const eir::Object& output = unit.objects[unit.ports.back().objectIndex];
    const std::optional<eir::BitVector> returned = run(unit);
    const bool agrees = returned.has_value() && output.initialValue.has_value()
                        && *returned == *output.initialValue;
    if (!agrees) {
        diagnostics->report(
            DiagCode::ConstantFunctionDisagrees, unit.location, unit.name,
            returned.has_value() ? eir::toString(returned->ref(), eir::Radix::Decimal)
                                 : std::string("nothing"),
            output.initialValue.has_value()
                ? eir::toString(output.initialValue->ref(), eir::Radix::Decimal)
                : std::string("nothing"));
    }
    checked.emplace(unit.name, agrees);
    return agrees;
}

std::optional<eir::BitVector>
ConstantFunctionRunner::run(const eir::ModuleDefinition& unit) const {
    // A function's own diagnostics — a `$warning` in its body, which a constant function's
    // evaluation ignores (IEEE 1800 section 13.4.3) — are not the design's, and neither is
    // anything it prints.
    DiagnosticEngine quiet(*sources);
    std::ostringstream discarded;

    eir::Design alone;
    alone.addModule(unit);
    elab::Elaborator elaborator(alone, quiet);
    const std::unique_ptr<elab::ElaboratedDesign> elaborated = elaborator.elaborate(unit.name);
    if (elaborated == nullptr) return std::nullopt;

    codegen::Compiler compiler(quiet);
    const std::unique_ptr<codegen::CompiledDesign> compiled = compiler.compile(elaborated->units());
    if (compiled == nullptr) return std::nullopt;

    const runtime::Image image(*elaborated, *compiled);
    runtime::KernelOptions options;
    options.transcript = &discarded;
    runtime::Kernel kernel(image, quiet, options);
    kernel.initialize();
    if (kernel.run() != runtime::RunStatus::Quiescent || quiet.errorCount() != 0)
        return std::nullopt;

    const eir::Object& output = unit.objects[unit.ports.back().objectIndex];
    const std::uint32_t signal =
        image.findSignal(std::format("/{}/{}", unit.sourceName, output.name));
    if (signal == runtime::Image::NoSignal) return std::nullopt;
    const runtime::Signal& stored = image.signals()[signal];
    return eir::BitVector::from(kernel.arena().at(stored.wordOffset, stored.bitWidth));
}

} // namespace enzan::session
