// SPDX-License-Identifier: GPL-3.0-or-later

#include "eir/Design.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using enzan::eir::Design;
using enzan::eir::Instance;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Type;

ModuleDefinition makeModule(std::string name) {
    ModuleDefinition definition;
    definition.sourceName = name;
    definition.name = std::move(name);
    return definition;
}

TEST(Design, StartsEmpty) {
    const Design design;
    EXPECT_TRUE(design.empty());
    EXPECT_EQ(design.size(), 0U);
    EXPECT_EQ(design.findModule("nothing"), nullptr);
    EXPECT_FALSE(design.findModuleIndex("nothing").has_value());
}

TEST(Design, AddAndFind) {
    Design design;
    const std::uint32_t index = design.addModule(makeModule("counter"));

    EXPECT_EQ(design.size(), 1U);
    EXPECT_EQ(index, 0U);
    ASSERT_NE(design.findModule("counter"), nullptr);
    EXPECT_EQ(design.findModule("counter")->name, "counter");
    EXPECT_EQ(design.findModuleIndex("counter"), 0U);
    EXPECT_EQ(&design.module(0), design.findModule("counter"));
}

TEST(Design, ReanalysingAModuleReplacesItInPlace) {
    // Re-analysing a changed source file must update the library rather than add a
    // second definition under the same name; the index stays stable so that anything
    // holding one is not invalidated.
    Design design;
    ModuleDefinition first = makeModule("counter");
    first.objects.push_back(Object{.name = "a", .type = Type::logic()});
    const std::uint32_t index = design.addModule(std::move(first));

    ModuleDefinition second = makeModule("counter");
    second.objects.push_back(Object{.name = "b", .type = Type::logic()});
    const std::uint32_t again = design.addModule(std::move(second));

    EXPECT_EQ(again, index);
    EXPECT_EQ(design.size(), 1U);
    ASSERT_EQ(design.module(index).objects.size(), 1U);
    EXPECT_EQ(design.module(index).objects[0].name, "b");
}

TEST(Design, CandidateTopModulesExcludesAnythingInstantiated) {
    Design design;
    design.addModule(makeModule("leaf"));

    ModuleDefinition middle = makeModule("middle");
    middle.instances.push_back(Instance{.name = "u_leaf", .definitionName = "leaf"});
    design.addModule(std::move(middle));

    ModuleDefinition top = makeModule("testbench");
    top.instances.push_back(Instance{.name = "u_middle", .definitionName = "middle"});
    design.addModule(std::move(top));

    EXPECT_EQ(design.candidateTopModules(), std::vector<std::string>{"testbench"});
}

TEST(Design, CandidateTopModulesIsOrderedByName) {
    // A driver picking a default top, or a diagnostic listing the choices, must be
    // reproducible across runs (DESIGN_GUIDE.md section 3.8), so the result is name
    // ordered rather than insertion ordered.
    Design design;
    design.addModule(makeModule("zulu"));
    design.addModule(makeModule("alpha"));
    design.addModule(makeModule("mike"));

    EXPECT_EQ(design.candidateTopModules(), std::vector<std::string>({"alpha", "mike", "zulu"}));
}

TEST(Design, EveryModuleIsACandidateWhenNothingIsInstantiated) {
    Design design;
    design.addModule(makeModule("one"));
    design.addModule(makeModule("two"));

    EXPECT_EQ(design.candidateTopModules().size(), 2U);
}

TEST(ModuleDefinition, FindObjectByName) {
    ModuleDefinition definition = makeModule("m");
    definition.addObject(Object{.name = "clk", .type = Type::logic()});
    const std::uint32_t index =
        definition.addObject(Object{.name = "count", .type = Type::bits(8, false, true)});

    EXPECT_EQ(definition.findObject("count"), index);
    EXPECT_EQ(definition.findObject("clk"), 0U);
    EXPECT_FALSE(definition.findObject("absent").has_value());
}

} // namespace
