// SPDX-License-Identifier: GPL-3.0-or-later
//
// VPI and VHPI on a design of both languages, built by hand: each interface shows its own
// language, and finds a name another interface made, whose start is that interface's path
// (DESIGN_GUIDE.md section 18, M7).

#include "pli/PliContext.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"

#include "support/Simulation.h"

#include "vhpi_user.h"
#include "vpi_user.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using enzan::eir::Design;
using enzan::eir::Instance;
using enzan::eir::Language;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Type;
using enzan::eir::VhdlType;
using enzan::pli::PliContext;
using enzan::test::Simulation;

/// The class cocotb's VHPI header names `vhpiVerilog`.
constexpr vhpiIntT VerilogRegion = 1117;

/// A definition of `language` called `name`, with a signal `sig` and the given instances.
ModuleDefinition definition(std::string name, Language language,
                            std::vector<Instance> instances = {}) {
    ModuleDefinition result;
    result.name = name;
    result.sourceName = std::move(name);
    result.language = language;
    Object signal{.name = "sig", .type = Type::bits(8, false, false)};
    if (language == Language::Vhdl) {
        result.vhdlTypes.push_back(
            VhdlType{.kind = VhdlType::Kind::Integer,
                     .name = "INTEGER",
                     .width = 8,
                     .ranges = {{.left = 0, .right = 255, .ascending = true}}});
        signal.vhdlType = 0;
    }
    result.addObject(std::move(signal));
    result.instances = std::move(instances);
    return result;
}

/// `Top`, of `top`'s language, holding `Child`, of the other, as `u_child`.
Design design(Language top) {
    const Language other = top == Language::Vhdl ? Language::SystemVerilog : Language::Vhdl;
    Design result;
    result.addModule(definition("Child", other));
    result.addModule(definition(
        "Top", top, {Instance{.name = "u_child", .definitionName = "Child", .connections = {}}}));
    return result;
}

class MixedLanguage : public ::testing::Test {
protected:
    MixedLanguage() : engine(sources), context(engine, {"enzan"}) {}

    void attach(Language top) {
        simulation = std::make_unique<Simulation>(design(top), "Top");
        ASSERT_TRUE(simulation->ready());
        context.attach(simulation->designImage(), simulation->simulationKernel());
    }

    static std::string vpiString(PLI_INT32 property, vpiHandle handle) {
        const char* text = vpi_get_str(property, handle);
        return text == nullptr ? std::string() : std::string(text);
    }

    static std::string vhpiString(vhpiStrPropertyT property, vhpiHandleT handle) {
        const auto* text = reinterpret_cast<const char*>(vhpi_get_str(property, handle));
        return text == nullptr ? std::string() : std::string(text);
    }

    std::unique_ptr<Simulation> simulation;
    enzan::SourceManager sources;
    enzan::DiagnosticEngine engine;
    PliContext context;
};

TEST_F(MixedLanguage, VpiFindsNothingOfVhdlByName) {
    attach(Language::SystemVerilog);
    EXPECT_NE(vpi_handle_by_name(const_cast<PLI_BYTE8*>("Top.sig"), nullptr), nullptr);
    EXPECT_EQ(vpi_handle_by_name(const_cast<PLI_BYTE8*>("Top.u_child"), nullptr), nullptr);
    EXPECT_EQ(vpi_handle_by_name(const_cast<PLI_BYTE8*>("Top.u_child.sig"), nullptr), nullptr);
}

TEST_F(MixedLanguage, VpiIteratesAVhdlInstanceAsAnObjectItDoesNotDescribe) {
    // Named, so that an application can ask the other interface for it, and with nothing in it.
    attach(Language::SystemVerilog);
    vpiHandle top = vpi_handle_by_name(const_cast<PLI_BYTE8*>("Top"), nullptr);
    ASSERT_NE(top, nullptr);
    vpiHandle iterator = vpi_iterate(vpiModule, top);
    ASSERT_NE(iterator, nullptr);
    vpiHandle child = vpi_scan(iterator);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(vpi_get(vpiType, child), vpiUnknown);
    EXPECT_EQ(vpiString(vpiName, child), "u_child");
    EXPECT_EQ(vpi_scan(iterator), nullptr);
    EXPECT_EQ(vpi_iterate(vpiNet, child), nullptr);
    EXPECT_EQ(vpi_iterate(vpiReg, child), nullptr);
}

TEST_F(MixedLanguage, VhpiShowsASystemVerilogInstanceAsAVerilogRegion) {
    attach(Language::Vhdl);
    vhpiHandleT root = vhpi_handle(vhpiRootInst, nullptr);
    ASSERT_NE(root, nullptr);
    vhpiHandleT iterator = vhpi_iterator(vhpiInternalRegions, root);
    ASSERT_NE(iterator, nullptr);
    vhpiHandleT child = vhpi_scan(iterator);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(vhpi_get(vhpiKindP, child), VerilogRegion);
    EXPECT_EQ(vhpiString(vhpiCaseNameP, child), "u_child") << "as SystemVerilog names it";
    EXPECT_EQ(vhpi_iterator(vhpiSigDecls, child), nullptr);
    EXPECT_EQ(vhpi_handle_by_name("TOP.U_CHILD.SIG", nullptr), nullptr);
}

TEST_F(MixedLanguage, VhpiHasNoRootUnderASystemVerilogTop) {
    attach(Language::SystemVerilog);
    EXPECT_EQ(vhpi_handle(vhpiRootInst, nullptr), nullptr);
}

TEST_F(MixedLanguage, VhpiFindsANameRootedAtASystemVerilogTop) {
    // As cocotb makes it from VPI's names, in upper case.
    attach(Language::SystemVerilog);
    vhpiHandleT child = vhpi_handle_by_name("TOP.U_CHILD", nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(vhpi_get(vhpiKindP, child), vhpiCompInstStmtK);
    vhpiHandleT signal = vhpi_handle_by_name("TOP.U_CHILD.SIG", nullptr);
    ASSERT_NE(signal, nullptr);
    EXPECT_EQ(vhpi_get(vhpiKindP, signal), vhpiSigDeclK);
    EXPECT_EQ(vhpi_handle_by_name("TOP.SIG", nullptr), nullptr) << "a SystemVerilog signal";
}

TEST_F(MixedLanguage, VpiFindsANameWhoseStartIsAVhdlPath) {
    // As cocotb makes it from VHPI's names: the VHDL top in upper case, the rest as VPI names it.
    attach(Language::Vhdl);
    EXPECT_EQ(vpi_iterate(vpiModule, nullptr), nullptr) << "VPI shows no VHDL top";
    vpiHandle child = vpi_handle_by_name(const_cast<PLI_BYTE8*>("TOP.u_child"), nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(vpi_get(vpiType, child), vpiModule);
    vpiHandle signal = vpi_handle_by_name(const_cast<PLI_BYTE8*>("TOP.u_child.sig"), nullptr);
    ASSERT_NE(signal, nullptr);
    EXPECT_EQ(vpi_handle_by_name(const_cast<PLI_BYTE8*>("TOP.U_CHILD.sig"), nullptr), nullptr)
        << "a SystemVerilog name is matched in its own case";
}

} // namespace
