// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "gui/KernelSession.h"
#include "gui/WaveformModel.h"
#include "gui/WaveformView.h"
#include "runtime/Time.h"

#include <QModelIndex>
#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

class QTimer;

namespace enzan::gui {

/// Finds when the selected wave row next starts to show a value, and puts the active cursor
/// there.
///
/// **A search never blocks the window.** No index says when a row holds a value, so a search
/// examines the row's changes one by one (`WaveformModel::nextMatchAfter`), and on a trace of
/// millions of them, or a group of such traces, that can take longer than a window may stop
/// answering for. So the first slice runs at once, which finishes most searches, and the rest run
/// a slice at a time on the event loop, as the session reads its socket. A reader can carry on
/// meanwhile, and moving the cursor or the selection abandons the search, as does changing the
/// rows the selected one is joined from, since its answer would be to a question no longer asked.
class ValueFinder : public QObject {
    Q_OBJECT

public:
    /// All three must outlive the finder.
    ValueFinder(const KernelSession& session, const WaveformModel& model, WaveformView& view,
                QObject* parent = nullptr);

    /// Searches the row the signal list has selected for `text`, read as the row shows a value,
    /// after the active cursor when `forward` and before it otherwise, and moves the cursor to the
    /// start of what it finds. A search still running is abandoned first.
    ///
    /// Nothing at or past the time the traces stop at is searched, since what a row shows there
    /// is not known yet, and a search does not wrap round. When there is nothing to search — no
    /// row, a parameter, a locked cursor, text that is no value of the row — it says why.
    void find(const QString& text, bool forward);

    /// Abandons the search running, if there is one, and leaves the cursor where it is.
    void cancel();

    [[nodiscard]] bool searching() const noexcept { return search.has_value(); }

    /// How many edges one call into the model examines, and how long a slice of calls may run
    /// before the window has a turn. A test sets a slice of a few edges, so that a search of a
    /// small design is long enough to be watched.
    void setSlice(int edgesPerCall, std::chrono::milliseconds duration);

Q_SIGNALS:
    /// A search finished, or could not start: what it found, that it found nothing, or why.
    void reported(const QString& message);

    /// A search goes on past its first slice.
    void searchStarted(const QString& message);

    /// A search running was abandoned before it finished.
    void searchAbandoned();

private:
    struct Search {
        int rowId = -1;
        eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);

        /// Its digits written `X` or `Z`, which ask what they say of the bits they cover.
        std::vector<PartialDigit> partial;

        /// The value as the reader typed it, for saying what was looked for.
        QString text;

        bool forward = true;

        /// The cursor the search moves and where it was, which a search abandons once either
        /// changes.
        int cursor = 0;
        runtime::SimTime cursorTime = 0;

        /// Where the next slice goes on from.
        runtime::SimTime from = 0;
    };

    /// Runs one slice of the search, and the next one on the event loop if it does not finish.
    void step();

    /// True while the search is still the one the reader asked for: the same row selected, and the
    /// same cursor, unlocked and where it was.
    [[nodiscard]] bool stillAsked(const Search& asked) const;

    /// Abandons the search when rows were added or taken away at `parent`, and it is the row
    /// searched or lies below it. The row's value is joined from the rows beneath it, so with one
    /// taken out, or put back in another place, the bits searched for stand for something else,
    /// though there may be as many of them.
    void rowsChangedAt(const QModelIndex& parent);

    void finish(const WaveformModel::Match& match);

    const KernelSession* session;
    const WaveformModel* model;
    WaveformView* view;

    std::optional<Search> search;

    /// Gives the window its turn between slices.
    QTimer* nextSlice;

    int edgesPerCall = 4096;

    /// A slice short enough that the window answers between two of them as though nothing were
    /// running.
    std::chrono::milliseconds sliceDuration{10};
};

} // namespace enzan::gui
