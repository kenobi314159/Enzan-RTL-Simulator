// SPDX-License-Identifier: GPL-3.0-or-later
//
// The order the VHDL frontend analyses its sources in: each after those declaring the units it
// names, and otherwise as given. Scanned, not analysed, so these need no NVC.

#include "frontend/vhdl/SourceOrder.h"

#include "common/SourceManager.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using enzan::BufferId;
using enzan::SourceManager;
using enzan::frontend::vhdl::dependencyOrder;

class SourceOrderTest : public ::testing::Test {
protected:
    /// Adds a source called `name` holding `text`.
    BufferId add(std::string name, std::string_view text) {
        return sources.addBuffer(std::move(name), std::string(text));
    }

    /// The names of `buffers`, in their order.
    [[nodiscard]] std::vector<std::string> namesOf(const std::vector<BufferId>& buffers) const {
        std::vector<std::string> names;
        names.reserve(buffers.size());
        for (const BufferId buffer : buffers) names.emplace_back(sources.bufferName(buffer));
        return names;
    }

    [[nodiscard]] std::vector<std::string> ordered(const std::vector<BufferId>& given,
                                                   std::string_view work = "work") const {
        return namesOf(dependencyOrder(given, sources, work));
    }

    SourceManager sources;
};

constexpr std::string_view Package =
    "package widths is\n  constant W : natural := 8;\nend package;\n";
constexpr std::string_view User = "use work.widths.all;\nentity user is\nend entity user;\n";

TEST_F(SourceOrderTest, AnOrderThatWorksIsKept) {
    const BufferId package = add("pkg.vhd", Package);
    const BufferId user = add("user.vhd", User);
    const BufferId other = add("other.vhd", "entity other is\nend;\narchitecture a of other is\n"
                                            "begin\nend;\n");
    EXPECT_EQ(ordered({package, other, user}),
              (std::vector<std::string>{"pkg.vhd", "other.vhd", "user.vhd"}));
}

TEST_F(SourceOrderTest, APackageComesBeforeWhatUsesIt) {
    const BufferId user = add("user.vhd", User);
    const BufferId other = add("other.vhd", "entity other is\nend;\n");
    const BufferId package = add("pkg.vhd", Package);
    EXPECT_EQ(ordered({user, other, package}),
              (std::vector<std::string>{"other.vhd", "pkg.vhd", "user.vhd"}));
}

TEST_F(SourceOrderTest, AnEntityComesBeforeItsArchitectureAndAPackageBeforeItsBody) {
    const BufferId architecture =
        add("arch.vhd", "architecture rtl of Counter is\nbegin\nend architecture;\n");
    const BufferId body = add("body.vhd", "package body widths is\nend package body widths;\n");
    const BufferId entity = add("ent.vhd", "entity counter is\n  port (clk : in bit);\nend;\n");
    const BufferId package = add("pkg.vhd", Package);
    EXPECT_EQ(ordered({architecture, body, entity, package}),
              (std::vector<std::string>{"ent.vhd", "arch.vhd", "pkg.vhd", "body.vhd"}));
}

TEST_F(SourceOrderTest, AnInstanceAndAConfigurationComeAfterTheEntityTheyName) {
    const BufferId top =
        add("top.vhd", "entity top is\nend;\narchitecture a of top is\nbegin\n"
                       "  u : entity work.leaf(rtl) port map (x => open);\nend;\n");
    const BufferId configuration =
        add("cfg.vhd", "configuration top_cfg of top is\n  for a\n  end for;\nend;\n");
    const BufferId leaf = add("leaf.vhd", "entity LEAF is\nend;\narchitecture rtl of leaf is\n"
                                          "begin\nend;\n");
    EXPECT_EQ(ordered({configuration, top, leaf}),
              (std::vector<std::string>{"leaf.vhd", "top.vhd", "cfg.vhd"}));
}

TEST_F(SourceOrderTest, NamesInCommentsStringsAndCharactersAreNotUnitsNamed) {
    const BufferId first = add("first.vhd", "-- use work.widths.all;\n/* entity work.widths */\n"
                                            "entity first is\nend;\narchitecture a of first is\n"
                                            "  constant s : string := \"work.widths\";\n"
                                            "  constant c : character := '.';\nbegin\nend;\n");
    const BufferId package = add("pkg.vhd", Package);
    EXPECT_EQ(ordered({first, package}), (std::vector<std::string>{"first.vhd", "pkg.vhd"}));
}

TEST_F(SourceOrderTest, TheWorkLibraryIsNamedByItsOwnNameToo) {
    const BufferId user = add("user.vhd", "library neorv32;\nuse neorv32.neorv32_package.all;\n"
                                          "entity user is\nend;\n");
    const BufferId package =
        add("pkg.vhd", "package neorv32_package is\nend package neorv32_package;\n");
    EXPECT_EQ(ordered({user, package}, "neorv32"),
              (std::vector<std::string>{"pkg.vhd", "user.vhd"}));
    EXPECT_EQ(ordered({user, package}, "work"), (std::vector<std::string>{"user.vhd", "pkg.vhd"}))
        << "another library's package is no prerequisite";
}

TEST_F(SourceOrderTest, SourcesNamingEachOtherStayAsGiven) {
    const BufferId first =
        add("a.vhd", "package a is\nend;\nuse work.b.all;\nentity ua is\nend;\n");
    const BufferId second =
        add("b.vhd", "package b is\nend;\nuse work.a.all;\nentity ub is\nend;\n");
    EXPECT_EQ(ordered({first, second}), (std::vector<std::string>{"a.vhd", "b.vhd"}));
}

} // namespace
