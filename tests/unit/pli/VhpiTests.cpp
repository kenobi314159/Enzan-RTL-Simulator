// SPDX-License-Identifier: GPL-3.0-or-later
//
// VHPI as an application sees it: the routines of vhpi_user.h, called on a design built by hand
// and run by a real kernel, as PliContextTests.cpp calls VPI's (DESIGN_GUIDE.md section 9.7).

#include "pli/PliContext.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"

#include "support/Literals.h"
#include "support/Simulation.h"

#include "vhpi_user.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using enzan::eir::Design;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Type;
using enzan::eir::VhdlType;
using enzan::pli::PliContext;
using enzan::test::literal;
using enzan::test::Simulation;

/// Where `state` is declared, the first buffer of the fixture's source manager.
constexpr std::string_view Declarations = "signal State : state_t;\n";

/// `tb`, holding `state` and `\Odd.Name\`, signals of `type state_t is (idle, busy, done)`, and
/// `limit`, an integer constant its architecture declares.
Design makeDesign() {
    ModuleDefinition top;
    top.name = "tb";
    top.sourceName = "tb";
    top.enums.push_back(
        enzan::eir::EnumDefinition{.name = "state_t",
                                   .members = {{.name = "idle", .value = literal("8'd0")},
                                               {.name = "busy", .value = literal("8'd1")},
                                               {.name = "done", .value = literal("8'd2")}}});
    top.vhdlTypes.push_back(VhdlType{.kind = VhdlType::Kind::Enumeration,
                                     .name = "STATE_T",
                                     .width = 8,
                                     .ranges = {{.left = 0, .right = 2, .ascending = true}},
                                     .enumIndex = 0});
    Object state{
        .name = "state", .type = Type::bits(8, false, false), .initialValue = literal("8'd0")};
    state.enumIndex = 0;
    state.vhdlType = 0;
    Object odd = state;
    state.location = enzan::SourceLocation(enzan::BufferId::fromIndex(0), 7);
    odd.name = "\\Odd.Name\\";
    top.addObject(std::move(state));
    top.addObject(std::move(odd));
    top.vhdlTypes.push_back(
        VhdlType{.kind = VhdlType::Kind::Integer,
                 .name = "INTEGER",
                 .width = 32,
                 .ranges = {{.left = -2147483647 - 1, .right = 2147483647, .ascending = true}}});
    top.parameters.push_back(enzan::eir::Parameter{
        .name = "limit", .value = literal("32'd7"), .isLocal = true, .vhdlType = 1});

    Design design;
    design.addModule(std::move(top));
    return design;
}

class Vhpi : public ::testing::Test {
protected:
    Vhpi() : simulation(makeDesign(), "tb"), engine(sources), context(engine, {"enzan"}) {
        sources.addBuffer("design.vhd", std::string(Declarations));
    }

    void SetUp() override {
        ASSERT_TRUE(simulation.ready());
        context.attach(simulation.designImage(), simulation.simulationKernel());
    }

    static int putEnumeration(vhpiHandleT handle, vhpiEnumT position) {
        vhpiValueT value{.format = vhpiEnumVal,
                         .bufSize = 0,
                         .numElems = 0,
                         .unit = {},
                         .value = {.enumv = position}};
        return vhpi_put_value(handle, &value, vhpiDepositPropagate);
    }

    static std::string lastError() {
        vhpiErrorInfoT info{
            .severity = vhpiNote, .message = nullptr, .str = nullptr, .file = nullptr, .line = 0};
        if (vhpi_check_error(&info) == 0 || info.message == nullptr) return {};
        return info.message;
    }

    Simulation simulation;
    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    PliContext context;
};

TEST_F(Vhpi, AnEnumerationTakesOnlyItsLiteralsPositions) {
    // A position past the last literal is no value of the type, and the design relies on there
    // being none: an index of an enumeration is not checked against the array's bounds.
    vhpiHandleT state = vhpi_handle_by_name("tb.state", nullptr);
    ASSERT_NE(state, nullptr) << lastError();

    EXPECT_EQ(putEnumeration(state, 2), 0) << lastError();
    EXPECT_EQ(putEnumeration(state, 3), -1);
    EXPECT_EQ(lastError(), "vhpi_put_value: ':tb:state' takes the positions of its 3 literals");
}

TEST_F(Vhpi, AnArchitecturesConstantIsAConstantWithAValue) {
    vhpiHandleT limit = vhpi_handle_by_name("tb.limit", nullptr);
    ASSERT_NE(limit, nullptr) << lastError();
    EXPECT_EQ(vhpi_get(vhpiKindP, limit), vhpiConstDeclK);
    vhpiValueT value{.format = vhpiIntVal, .bufSize = 0, .numElems = 0, .unit = {}, .value = {}};
    ASSERT_EQ(vhpi_get_value(limit, &value), 0) << lastError();
    EXPECT_EQ(value.value.intg, 7);

    value.value.intg = 8;
    EXPECT_EQ(vhpi_put_value(limit, &value, vhpiDepositPropagate), -1);
    EXPECT_EQ(lastError(), "vhpi_put_value: ':tb:limit' cannot be written");
}

TEST_F(Vhpi, ADeclaredObjectsCaseNameIsSpelledAsItsDeclarationSpellsIt) {
    vhpiHandleT state = vhpi_handle_by_name("tb.state", nullptr);
    ASSERT_NE(state, nullptr) << lastError();
    const auto string = [](vhpiStrPropertyT property, vhpiHandleT handle) {
        return std::string(reinterpret_cast<const char*>(vhpi_get_str(property, handle)));
    };
    EXPECT_EQ(string(vhpiNameP, state), "STATE");
    EXPECT_EQ(string(vhpiCaseNameP, state), "State");
    EXPECT_EQ(string(vhpiFullCaseNameP, state), ":TB:State");

    // Nothing to read the spelling from.
    vhpiHandleT limit = vhpi_handle_by_name("tb.limit", nullptr);
    ASSERT_NE(limit, nullptr) << lastError();
    EXPECT_EQ(string(vhpiCaseNameP, limit), "LIMIT");
}

TEST_F(Vhpi, AnExtendedIdentifierIsOneNameAsWritten) {
    // Its dot names no region, and its case is its own.
    vhpiHandleT root = vhpi_handle(vhpiRootInst, nullptr);
    ASSERT_NE(root, nullptr) << lastError();
    EXPECT_EQ(vhpi_iterator(vhpiInternalRegions, root), nullptr) << "no region `\\Odd`";

    std::vector<std::string> names;
    vhpiHandleT iterator = vhpi_iterator(vhpiSigDecls, root);
    ASSERT_NE(iterator, nullptr) << lastError();
    while (vhpiHandleT signal = vhpi_scan(iterator))
        names.emplace_back(reinterpret_cast<const char*>(vhpi_get_str(vhpiNameP, signal)));
    EXPECT_EQ(names, (std::vector<std::string>{"STATE", "\\Odd.Name\\"}));
}

TEST_F(Vhpi, AStringReturnedStaysValidWhenAnotherIsAskedFor) {
    // cocotb formats two of them into one message, the second asked for before the first is read.
    vhpiHandleT state = vhpi_handle_by_name("tb.state", nullptr);
    ASSERT_NE(state, nullptr) << lastError();
    vhpiHandleT root = vhpi_handle(vhpiRootInst, nullptr);
    ASSERT_NE(root, nullptr) << lastError();

    const auto* name = reinterpret_cast<const char*>(vhpi_get_str(vhpiNameP, state));
    const auto* kind = reinterpret_cast<const char*>(vhpi_get_str(vhpiKindStrP, root));
    const auto* full = reinterpret_cast<const char*>(vhpi_get_str(vhpiFullNameP, state));
    const auto* again = reinterpret_cast<const char*>(vhpi_get_str(vhpiNameP, state));
    ASSERT_NE(name, nullptr);
    ASSERT_NE(kind, nullptr);
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(std::string(name), "STATE");
    EXPECT_EQ(std::string(kind), "vhpiRootInstK");
    EXPECT_EQ(std::string(full), ":TB:STATE");
    EXPECT_EQ(again, name) << "the same handle's name is the same string";
}

} // namespace
