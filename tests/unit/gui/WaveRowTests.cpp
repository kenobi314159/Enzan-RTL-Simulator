// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for what a row of the wave window shows, and for the clipboard.
//
// The rule being pinned here is the one in section 10.7 that a row with children shows what its
// children show, joined together. It looks like a small thing and it is doing three jobs at
// once: a bus that has had some of its bits deleted, a group of one-bit signals standing in for
// a vector, and the nesting of those two that gives multi-dimensional arrays. Written as three
// features it would have been three sets of edge cases; written as one rule it needs one.

#include "gui/WaveformModel.h"

#include "gui/MainWindow.h"
#include "gui/Theme.h"
#include "gui/WaveformView.h"

#include "SessionFixture.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QTableView>
#include <QTreeView>

#include <memory>

namespace {

using enzan::eir::Radix;
using enzan::gui::MainWindow;
using enzan::gui::SignalInfo;
using enzan::gui::Theme;
using enzan::gui::WaveformModel;
using enzan::gui::WaveRow;
using enzan::test::SessionFixture;

class WaveRowTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        model = std::make_unique<WaveformModel>(*session, theme);

        // A run behind the window, so that the rows have values to join.
        clock = infoFor(QStringLiteral("clk"));
        counter = infoFor(QStringLiteral("count"));
        session->subscribe({clock.signalIndex, counter.signalIndex});
        session->run(400);
        pumpUntil([this] { return session->currentTime() == 400; });
    }

    void TearDown() override {
        model = nullptr;
        SessionFixture::TearDown();
    }

    SignalInfo infoFor(const QString& name) {
        for (const SignalInfo& info : objectsOf(QStringLiteral("/tb"))) {
            if (info.name == name) return info;
        }
        ADD_FAILURE() << "no signal " << name.toStdString();
        return {};
    }

    /// The row at `position` under `parent`.
    [[nodiscard]] int childOf(int parent, int position) const {
        return model->rowIdOf(model->index(position, 0, model->indexOfRow(parent)));
    }

    Theme theme;
    std::unique_ptr<WaveformModel> model;
    SignalInfo clock;
    SignalInfo counter;
};

// ---------------------------------------------------------------------------
// A row with children shows what its children show
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, AnUnpackedBusStillShowsItsOwnValue) {
    // With every bit present, joining them gives the value the signal already had. Nothing
    // about the row changes when it is opened, which is what makes unpacking free to try.
    const int bus = model->addSignal(counter);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));

    model->unpack(bus);

    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));
    EXPECT_EQ(model->displayedWidth(bus), 8U);
    EXPECT_TRUE(model->childrenReconstituteParent(bus))
        << "an untouched unpacking must be recognised, or the row pays to be drawn twice";
}

TEST_F(WaveRowTest, RemovingABitNarrowsWhatTheBusShows) {
    // Section 10.7: the parent shows what is left of it. The counter holds 40 at 400ns —
    // 0b00101000 — so dropping bit 3, the set bit, leaves 0b0010000, which is 0x10.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int bit3 = childOf(bus, 4);
    ASSERT_NE(model->row(bit3), nullptr);
    ASSERT_EQ(model->row(bit3)->label, QStringLiteral("count[3]"));

    model->removeRow(bit3);

    EXPECT_EQ(model->displayedWidth(bus), 7U);
    EXPECT_FALSE(model->childrenReconstituteParent(bus));
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("10"));
}

TEST_F(WaveRowTest, RemovingEveryBitLeavesTheBusWithNothingToShow) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    while (model->rowCount(model->indexOfRow(bus)) > 0) model->removeRow(childOf(bus, 0));

    EXPECT_EQ(model->displayedWidth(bus), 0U);
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("—"));
}

TEST_F(WaveRowTest, AGroupOfOneBitSignalsBehavesAsAVector) {
    // Four bits of the counter, collected by hand into a group, read as a four-bit number.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int group = model->addGroup(QStringLiteral("nibble"));
    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, group, bit));
    }

    // Bits 7 down to 4 of 40 are 0b0010, which is 2.
    EXPECT_EQ(model->displayedWidth(group), 4U);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("2"));

    model->setRadix(group, Radix::Binary);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("0010"));
}

TEST_F(WaveRowTest, AGroupOfMixedWidthsShowsNothing) {
    // A number joined out of signals that have nothing to do with each other is noise, so a
    // grab-bag group stays blank rather than inventing a value for itself.
    const int group = model->addGroup(QStringLiteral("odds and ends"));
    const int oneBit = model->addSignal(clock, group);
    const int eightBit = model->addSignal(counter, group);

    ASSERT_NE(model->row(oneBit), nullptr);
    ASSERT_NE(model->row(eightBit), nullptr);

    EXPECT_TRUE(model->renderValue(group, 400).isEmpty());
}

TEST_F(WaveRowTest, AnEmptyGroupShowsNothing) {
    const int group = model->addGroup(QStringLiteral("empty"));
    EXPECT_TRUE(model->renderValue(group, 400).isEmpty());
    EXPECT_EQ(model->displayedWidth(group), 0U);
}

TEST_F(WaveRowTest, GroupsNestIntoSomethingWider) {
    // The multi-dimensional case: two four-bit groups inside one, which is eight bits. It
    // works because the rule is recursive, not because anything here knows about arrays.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int outer = model->addGroup(QStringLiteral("word"));
    const int high = model->addGroup(QStringLiteral("high"), outer);
    const int low = model->addGroup(QStringLiteral("low"), outer);

    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, high, bit));
    }
    for (int bit = 0; bit < 4; ++bit) {
        ASSERT_TRUE(model->moveRowsTo({childOf(bus, 0)}, low, bit));
    }

    EXPECT_EQ(model->displayedWidth(high), 4U);
    EXPECT_EQ(model->displayedWidth(low), 4U);
    EXPECT_EQ(model->displayedWidth(outer), 8U);
    EXPECT_EQ(model->renderValue(outer, 400), QStringLiteral("28"))
        << "the two halves put back together are the counter again";
}

TEST_F(WaveRowTest, TheFirstChildIsTheMostSignificant) {
    // Which end is which is not a detail: reversed, every joined value would be wrong and
    // would still look plausible.
    const int bus = model->addSignal(counter);
    model->unpack(bus);

    const int group = model->addGroup(QStringLiteral("two"));
    ASSERT_TRUE(model->moveRowsTo({childOf(bus, 2)}, group, 0)); // count[5], which is 1
    ASSERT_TRUE(model->moveRowsTo({childOf(bus, 2)}, group, 1)); // count[4], which is 0

    model->setRadix(group, Radix::Binary);
    EXPECT_EQ(model->renderValue(group, 400), QStringLiteral("10"));
}

TEST_F(WaveRowTest, AJoinedValueTracksTheCursorLikeAnyOther) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);
    model->removeRow(childOf(bus, 0)); // drop bit 7, which is zero throughout

    // The counter reaches 20 at 200ns and 40 at 400ns; dropping the top bit changes neither.
    EXPECT_EQ(model->renderValue(bus, 200), QStringLiteral("14"));
    EXPECT_EQ(model->renderValue(bus, 400), QStringLiteral("28"));
}

// ---------------------------------------------------------------------------
// The clipboard
// ---------------------------------------------------------------------------

TEST_F(WaveRowTest, ARowCopiesAndPastes) {
    const int original = model->addSignal(counter);
    model->setRadix(original, Radix::Binary);
    model->setHighlight(original, 3);

    model->copyRows({original});
    EXPECT_TRUE(WaveformModel::clipboardHasRows());

    const QVector<int> pasted = model->pasteRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    const WaveRow* copy = model->row(pasted.front());
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->label, model->row(original)->label);
    EXPECT_EQ(copy->signalIndex, counter.signalIndex);
    EXPECT_EQ(copy->radix, Radix::Binary) << "a copy keeps how it was set up";
    EXPECT_EQ(copy->highlight, 3);
    EXPECT_EQ(model->rowCount(), 2);
}

TEST_F(WaveRowTest, CopyingAGroupCopiesWhatIsInIt) {
    const int group = model->addGroup(QStringLiteral("Bus"));
    model->addSignal(clock, group);
    model->addSignal(counter, group);

    model->copyRows({group});
    const QVector<int> pasted = model->pasteRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    const int copiedGroup = pasted.front();
    ASSERT_NE(model->row(copiedGroup), nullptr);
    EXPECT_EQ(model->row(copiedGroup)->kind, WaveRow::Kind::Group);
    EXPECT_EQ(model->rowCount(model->indexOfRow(copiedGroup)), 2);
    EXPECT_EQ(model->rowCount(), 2) << "the copy sits beside the original, not inside it";
}

TEST_F(WaveRowTest, AnUnpackedBusCopiesWithTheBitsItHasLeft) {
    const int bus = model->addSignal(counter);
    model->unpack(bus);
    model->removeRow(childOf(bus, 4));

    model->copyRows({bus});
    const QVector<int> pasted = model->pasteRows(-1, -1);
    ASSERT_EQ(pasted.size(), 1);

    EXPECT_EQ(model->rowCount(model->indexOfRow(pasted.front())), 7);
    EXPECT_EQ(model->renderValue(pasted.front(), 400), QStringLiteral("10"))
        << "a pasted row shows what the one it came from showed";
}

TEST_F(WaveRowTest, PastingIntoAGroupPutsTheRowsInside) {
    const int original = model->addSignal(clock);
    const int group = model->addGroup(QStringLiteral("Bus"));

    model->copyRows({original});
    const QVector<int> pasted = model->pasteRows(group, -1);

    ASSERT_EQ(pasted.size(), 1);
    EXPECT_EQ(model->row(pasted.front())->parent, group);
}

TEST_F(WaveRowTest, PastingAtAPositionLandsThere) {
    const int first = model->addSignal(clock);
    model->addSignal(counter);

    model->copyRows({first});
    const QVector<int> pasted = model->pasteRows(-1, 1);

    ASSERT_EQ(pasted.size(), 1);
    EXPECT_EQ(model->indexOfRow(pasted.front()).row(), 1)
        << "pasting beside the row you copied must not send it to the end";
}

TEST_F(WaveRowTest, PastingSubscribesToWhatArrived) {
    const int original = model->addSignal(counter);
    model->copyRows({original});
    model->removeRow(original);
    ASSERT_TRUE(model->displayedSignals().isEmpty());

    model->pasteRows(-1, -1);
    EXPECT_EQ(model->displayedSignals(), QVector<std::uint32_t>{counter.signalIndex});
}

TEST_F(WaveRowTest, PastingAnEmptyClipboardDoesNothing) {
    QApplication::clipboard()->clear();
    QApplication::clipboard()->setText(QStringLiteral("not a wave row"));

    EXPECT_FALSE(WaveformModel::clipboardHasRows());
    EXPECT_TRUE(model->pasteRows(-1, -1).isEmpty());
    EXPECT_EQ(model->rowCount(), 0);
}

TEST_F(WaveRowTest, CopyingAlsoLeavesThePathsAsText) {
    // So that the clipboard is worth something outside this window too.
    const int original = model->addSignal(counter);
    model->copyRows({original});

    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("/tb/count"));
}

// ---------------------------------------------------------------------------
// Adding a signal leaves the window as it was
// ---------------------------------------------------------------------------

/// The window itself, because this is about what the view does when rows arrive.
class WaveWindowTest : public SessionFixture {
protected:
    void SetUp() override {
        SessionFixture::SetUp();

        theme.loadBuiltIn();
        window = std::make_unique<MainWindow>(*session, theme);
        window->resize(1200, 700);
        window->show();
        pump(6);

        waveRows = window->findChildren<QTreeView*>().value(1);
        objects = window->findChild<QTableView*>();
        ASSERT_NE(waveRows, nullptr);
        ASSERT_NE(objects, nullptr);

        waveModel = qobject_cast<WaveformModel*>(waveRows->model());
        ASSERT_NE(waveModel, nullptr);
    }

    void TearDown() override {
        window = nullptr;
        SessionFixture::TearDown();
    }

    /// Selects one object by name in the Objects window and runs "Add to Wave".
    void addToWave(const QString& name) {
        for (int row = 0; row < objects->model()->rowCount(); ++row) {
            if (objects->model()->index(row, 0).data().toString() != name) continue;
            objects->selectionModel()->clearSelection();
            objects->selectRow(row);
        }

        for (QAction* action : objects->actions()) {
            if (action->text().contains(QStringLiteral("Add to Wave"))) action->trigger();
        }
        pump(2);
    }

    Theme theme;
    std::unique_ptr<MainWindow> window;
    QTreeView* waveRows = nullptr;
    QTableView* objects = nullptr;
    WaveformModel* waveModel = nullptr;
};

TEST_F(WaveWindowTest, ANewSignalArrivesPackedAndLeavesTheOthersAlone) {
    // Adding a signal used to expand the whole window, which unpacked every bus in it and threw
    // away whatever the reader had chosen to have open.
    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 1);

    const int bus = waveModel->rowIdOf(waveModel->index(0, 0));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(bus))) << "a signal arrives packed";

    waveRows->expand(waveModel->indexOfRow(bus));
    pump(2);
    ASSERT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)));
    ASSERT_EQ(waveModel->rowCount(waveModel->indexOfRow(bus)), 8);

    addToWave(QStringLiteral("clk"));

    ASSERT_EQ(waveModel->rowCount(), 2);
    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(bus)))
        << "what was already open must stay open";

    const int added = waveModel->rowIdOf(waveModel->index(1, 0));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(added)));
}

TEST_F(WaveWindowTest, ASecondBusArrivesPackedWhileTheFirstStaysOpen) {
    addToWave(QStringLiteral("count"));
    const int first = waveModel->rowIdOf(waveModel->index(0, 0));
    waveRows->expand(waveModel->indexOfRow(first));
    pump(2);

    addToWave(QStringLiteral("count"));
    ASSERT_EQ(waveModel->rowCount(), 2);

    const int second = waveModel->rowIdOf(waveModel->index(1, 0));
    EXPECT_TRUE(waveRows->isExpanded(waveModel->indexOfRow(first)));
    EXPECT_FALSE(waveRows->isExpanded(waveModel->indexOfRow(second)));
    EXPECT_FALSE(waveModel->row(second)->unpacked)
        << "the second copy of a bus must not inherit the first one's unpacking";
}

} // namespace
