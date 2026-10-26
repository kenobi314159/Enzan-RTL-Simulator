// SPDX-License-Identifier: GPL-3.0-or-later

#include "elab/Link.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Type.h"
#include "eir/UnitRequest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::Design;
using enzan::eir::ModuleDefinition;
using enzan::eir::Object;
using enzan::eir::Port;
using enzan::eir::PortConnection;
using enzan::eir::PortDirection;
using enzan::eir::Type;
using enzan::eir::UnitRequest;
using enzan::elab::linkRequests;

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { codes.push_back(diagnostic.code); }

    std::vector<DiagCode> codes;
};

/// A design of two definitions as the two frontends leave them: `adder$W=8`, which a request was
/// answered with, with the ports `a`, `b` and `Sum` in that order, and `tb`, whose instance `u`
/// names the request's placeholder and connects the request's ports `sum` and `a`, in the order the
/// component met them.
class LinkTest : public ::testing::Test {
protected:
    LinkTest() : engine(sources) {
        engine.addConsumer(consumer);

        ModuleDefinition adder;
        adder.name = "adder$W=8";
        for (const char* name : {"a", "b", "Sum"}) {
            const std::uint32_t object =
                adder.addObject(Object{.name = name, .type = Type::bits(8, false, true)});
            adder.ports.push_back(Port{.name = name,
                                       .direction = std::string(name) == "Sum"
                                                        ? PortDirection::Output
                                                        : PortDirection::Input,
                                       .objectIndex = object});
        }
        design.addModule(std::move(adder));

        ModuleDefinition tb;
        tb.name = "tb";
        tb.language = enzan::eir::Language::Vhdl;
        tb.addObject(Object{.name = "total", .type = Type::bits(64, false, false)});
        tb.addObject(Object{.name = "left", .type = Type::bits(64, false, false)});
        enzan::eir::Instance instance;
        instance.name = "u";
        instance.definitionName = "adder #(.w(8))";
        instance.connections.push_back(PortConnection{.portIndex = 0, .objectIndex = 0});
        instance.connections.push_back(PortConnection{.portIndex = 1, .objectIndex = 1});
        tb.instances.push_back(std::move(instance));
        design.addModule(std::move(tb));

        request = UnitRequest{.placeholder = "adder #(.w(8))",
                              .unit = "adder",
                              .parameters = {{.name = "w", .value = "8"}},
                              .ports = {{.name = "sum", .direction = PortDirection::Output},
                                        {.name = "a", .direction = PortDirection::Input}}};
    }

    bool link() {
        const std::vector<UnitRequest> requests{request};
        const std::vector<std::string> answers{"adder$W=8"};
        return linkRequests(design, requests, answers, engine);
    }

    [[nodiscard]] const enzan::eir::Instance& instance() const {
        return design.findModule("tb")->instances.front();
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;
    Design design;
    UnitRequest request;
};

TEST_F(LinkTest, AnInstanceNamesTheDefinitionItsRequestWasAnsweredWith) {
    ASSERT_TRUE(link());
    EXPECT_EQ(instance().definitionName, "adder$W=8");
    EXPECT_TRUE(consumer.codes.empty());
}

TEST_F(LinkTest, EachConnectionIsOfTheDefinitionsPortOfItsNameInAnyCase) {
    ASSERT_TRUE(link());
    ASSERT_EQ(instance().connections.size(), 2U);
    EXPECT_EQ(instance().connections[0].portIndex, 2U);
    EXPECT_EQ(instance().connections[0].objectIndex, 0U);
    EXPECT_EQ(instance().connections[1].portIndex, 0U);
    EXPECT_EQ(instance().connections[1].objectIndex, 1U);
}

TEST_F(LinkTest, APortTheDefinitionLacksIsRefusedByName) {
    request.ports[1].name = "carry";
    EXPECT_FALSE(link());
    EXPECT_EQ(consumer.codes, std::vector{DiagCode::RequestedPortMissing});
}

TEST_F(LinkTest, APortConnectedInAnotherDirectionIsRefused) {
    request.ports[1].direction = PortDirection::Output;
    EXPECT_FALSE(link());
    EXPECT_EQ(consumer.codes, std::vector{DiagCode::RequestedPortDirection});
}

TEST_F(LinkTest, AnInstanceOfNoRequestIsLeftAlone) {
    request.placeholder = "other #()";
    ASSERT_TRUE(link());
    EXPECT_EQ(instance().definitionName, "adder #(.w(8))");
    EXPECT_EQ(instance().connections[0].portIndex, 0U);
}

} // namespace
