// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/ValueFinder.h"

#include "gui/TimeAxis.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace enzan::gui {

ValueFinder::ValueFinder(const KernelSession& kernelSession, const WaveformModel& waveModel,
                         WaveformView& waveView, QObject* parent)
    : QObject(parent),
      session(&kernelSession),
      model(&waveModel),
      view(&waveView),
      nextSlice(new QTimer(this)) {
    nextSlice->setSingleShot(true);
    nextSlice->setInterval(0);
    connect(nextSlice, &QTimer::timeout, this, &ValueFinder::step);

    // A move is a removal and an insertion, and clearing the list is a reset.
    connect(model, &QAbstractItemModel::rowsInserted, this, &ValueFinder::rowsChangedAt);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &ValueFinder::rowsChangedAt);
    connect(model, &QAbstractItemModel::modelReset, this, &ValueFinder::cancel);
}

void ValueFinder::setSlice(int edges, std::chrono::milliseconds duration) {
    edgesPerCall = std::max(edges, 1);
    sliceDuration = duration;
}

void ValueFinder::find(const QString& text, bool forward) {
    cancel();

    const int rowId = view->selectedRow();
    const WaveRow* waveRow = model->row(rowId);
    if (waveRow == nullptr) {
        Q_EMIT reported(tr("Select a row to find a value in"));
        return;
    }
    if (waveRow->kind == WaveRow::Kind::Parameter) {
        Q_EMIT reported(
            tr("%1 is a parameter, which shows one value at every time").arg(waveRow->label));
        return;
    }

    const QVector<WaveCursor>& cursors = view->cursors();
    if (cursors.isEmpty()) {
        Q_EMIT reported(tr("There is no cursor to move"));
        return;
    }
    const int cursor = std::clamp(view->activeCursor(), 0, static_cast<int>(cursors.size()) - 1);
    if (cursors[cursor].locked) {
        Q_EMIT reported(tr("The active cursor is locked"));
        return;
    }

    ValueReading reading = model->readValue(rowId, text);
    if (!reading.value.has_value()) {
        Q_EMIT reported(reading.problem);
        return;
    }

    search = Search{.rowId = rowId,
                    .value = std::move(*reading.value),
                    .partial = std::move(reading.partial),
                    .text = text.trimmed(),
                    .forward = forward,
                    .cursor = cursor,
                    .cursorTime = cursors[cursor].time,
                    .from = cursors[cursor].time};
    step();
}

void ValueFinder::cancel() {
    if (!search.has_value()) return;

    search.reset();
    nextSlice->stop();
    Q_EMIT searchAbandoned();
}

void ValueFinder::rowsChangedAt(const QModelIndex& parent) {
    if (search.has_value() && model->isDescendantOf(model->rowIdOf(parent), search->rowId))
        cancel();
}

bool ValueFinder::stillAsked(const Search& asked) const {
    if (model->row(asked.rowId) == nullptr || view->selectedRow() != asked.rowId) return false;

    const QVector<WaveCursor>& cursors = view->cursors();
    return view->activeCursor() == asked.cursor && asked.cursor < cursors.size()
           && cursors[asked.cursor].time == asked.cursorTime && !cursors[asked.cursor].locked;
}

void ValueFinder::step() {
    if (!search.has_value()) return;
    if (!stillAsked(*search)) {
        cancel();
        return;
    }
    Search& current = *search;

    const bool continuing = current.from != current.cursorTime;

    QElapsedTimer clock;
    clock.start();
    WaveformModel::Match match;
    do {
        // Read afresh each call: a run in progress moves it, and a search may go as far as the
        // traces have got by the time it gets there.
        const runtime::SimTime limit = session->heldTime();
        match = current.forward
                    ? model->nextMatchAfter(current.rowId, current.from, current.value, limit,
                                            edgesPerCall, current.partial)
                    : model->previousMatchBefore(current.rowId, current.from, current.value, limit,
                                                 edgesPerCall, current.partial);
        if (match.outcome == WaveformModel::Match::Outcome::Unfinished) current.from = match.time;
    } while (match.outcome == WaveformModel::Match::Outcome::Unfinished
             && std::chrono::milliseconds(clock.elapsed()) < sliceDuration);

    if (match.outcome != WaveformModel::Match::Outcome::Unfinished) {
        finish(match);
        return;
    }

    if (!continuing) {
        Q_EMIT searchStarted(
            tr("Searching %1 for %2…").arg(model->row(current.rowId)->label, current.text));
    }
    nextSlice->start();
}

void ValueFinder::finish(const WaveformModel::Match& match) {
    if (!search.has_value()) return;
    const Search done = std::move(*search);
    search.reset();

    const QString label = model->row(done.rowId)->label;
    if (match.outcome == WaveformModel::Match::Outcome::Found) {
        view->placeActiveCursor(match.time);
        Q_EMIT reported(
            tr("%1 shows %2 from %3")
                .arg(label, done.text, TimeAxis::format(match.time, session->timeScale())));
        return;
    }

    if (done.forward) {
        Q_EMIT reported(tr("No later %1 in %2, up to %3")
                            .arg(done.text, label,
                                 TimeAxis::format(session->heldTime(), session->timeScale())));
    } else {
        Q_EMIT reported(tr("No earlier %1 in %2").arg(done.text, label));
    }
}

} // namespace enzan::gui
