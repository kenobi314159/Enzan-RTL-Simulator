// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sets up the one thing every test here needs: a QApplication, on the offscreen platform.
//
// The platform is chosen from inside the binary rather than from the environment the test is run
// in, so that the suite behaves the same whether CI runs it on a machine with no display at all
// or a contributor runs it by hand inside a desktop session. A GUI test that only passes when
// somebody happens to be logged in is a GUI test nobody runs.

#include <QApplication>

#include <gtest/gtest.h>

#include <array>

namespace {

/// Creates the QApplication before the first test and tears it down after the last.
class QtEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // Qt keeps a reference to argc, so both have to outlive the application object.
        application = std::make_unique<QApplication>(argumentCount, arguments.data());
    }

    void TearDown() override { application.reset(); }

private:
    int argumentCount = 1;
    std::array<char*, 2> arguments{const_cast<char*>("enzan-gui-tests"), nullptr};
    std::unique_ptr<QApplication> application;
};

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new QtEnvironment);
    return RUN_ALL_TESTS();
}
