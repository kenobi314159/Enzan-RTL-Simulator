// SPDX-License-Identifier: GPL-3.0-or-later

#include "gui/SourceView.h"

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QVBoxLayout>

namespace enzan::gui {

namespace {

/// Space each side of the line numbers, in character widths.
constexpr int GutterPadding = 2;

/// Diameter of a breakpoint dot, as a fraction of the line height.
constexpr double BreakpointSize = 0.55;

} // namespace

class SourceView::TextArea : public QPlainTextEdit {
public:
    explicit TextArea(QWidget* parent) : QPlainTextEdit(parent) {}

    using QPlainTextEdit::blockBoundingGeometry;
    using QPlainTextEdit::blockBoundingRect;
    using QPlainTextEdit::contentOffset;
    using QPlainTextEdit::firstVisibleBlock;
};

/// The line-number strip, painted by the view it belongs to.
///
/// A sibling widget rather than a margin inside the text area: a `QPlainTextEdit` scrolls by
/// moving the pixels it already has, and a margin drawn inside it is repainted for every line
/// scrolled. This is painted only when its own contents change.
class SourceView::Gutter : public QWidget {
public:
    explicit Gutter(SourceView& owner) : QWidget(&owner), view(&owner) {}

    [[nodiscard]] QSize sizeHint() const override { return {view->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { view->paintGutter(event); }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        const std::uint32_t line = view->lineAt(event->position().toPoint().y());
        if (line == 0) return;

        // One gesture, two meanings, decided by what is already there. A gutter that needed a
        // modifier to remove a breakpoint would be a thing to remember; a click that toggles
        // is what every editor does.
        const Marker* existing = view->markerAt(line);
        if (existing != nullptr) {
            Q_EMIT view->breakpointRemoved(existing->id);
            return;
        }
        Q_EMIT view->breakpointRequested(view->currentFile(), line);
    }

private:
    SourceView* view;
};

SourceView::SourceView(const Theme& themeToUse, QWidget* parent)
    : QWidget(parent), theme(&themeToUse) {
    chooser = new QComboBox(this);
    chooser->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(chooser, &QComboBox::currentTextChanged, this, [this](const QString& path) {
        if (updatingChooser) return;
        showFile(path);
    });

    text = new TextArea(this);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    // A palette rather than a style sheet, because a style sheet that gives a widget a
    // background makes Qt repaint it rather than scroll it (section 10.5), and this is a
    // scrolling surface.
    QPalette palette = text->palette();
    palette.setColor(QPalette::Base, theme->color(ThemeColor::SourceBackground));
    palette.setColor(QPalette::Text, theme->color(ThemeColor::SourceForeground));
    text->setPalette(palette);

    gutter = new Gutter(*this);

    auto* rows = new QHBoxLayout;
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(0);
    rows->addWidget(gutter);
    rows->addWidget(text);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(chooser);
    layout->addLayout(rows);

    // The gutter follows the text's scroll position and its block count, which are the only two
    // things that change what it shows.
    connect(text->verticalScrollBar(), &QScrollBar::valueChanged, gutter,
            [this](int) { gutter->update(); });
    connect(text, &QPlainTextEdit::textChanged, this, &SourceView::refreshGutter);
    connect(text, &QPlainTextEdit::updateRequest, gutter, [this](const QRect&, int dy) {
        if (dy != 0) gutter->update();
    });
}

void SourceView::setFiles(const QStringList& files) {
    const QString keep = shownFile;

    // A new list means a new design, and the same path may well hold different bytes: editing a
    // file and analysing it again is the ordinary way that happens. Text kept against the path
    // would then be the *previous* design's, and the stopped-line marker would be drawn on a
    // line of it — a debugger pointing confidently somewhere wrong, which is the failure this
    // window takes its text from the kernel to avoid in the first place.
    cachedText.clear();

    updatingChooser = true;
    chooser->clear();
    chooser->addItems(files);
    updatingChooser = false;

    if (files.isEmpty()) return;

    // The file on show stays on show when it is still one of the design's, because a reader who
    // was looking at a line did not ask to be moved.
    if (!keep.isEmpty() && files.contains(keep)) {
        showFile(keep);
        return;
    }
    showFile(files.front());
}

void SourceView::showFile(const QString& path) {
    if (path.isEmpty()) return;

    shownFile = path;

    updatingChooser = true;
    const int index = chooser->findText(path);
    if (index >= 0) chooser->setCurrentIndex(index);
    updatingChooser = false;

    const auto cached = cachedText.constFind(path);
    if (cached == cachedText.constEnd()) {
        // Asked for, not read. The kernel holds the bytes it analysed, and it may not be on
        // this machine at all.
        text->setPlainText(tr("Loading %1…").arg(path));
        Q_EMIT fileRequested(path);
        return;
    }

    text->setPlainText(*cached);
    refreshGutter();
    refreshCurrentLine();
}

void SourceView::setFileText(const QString& path, const QString& contents, bool found) {
    cachedText.insert(path, found ? contents
                                  : tr("The kernel has no source called %1.\n\n"
                                       "Only the files the design was analysed from can be "
                                       "shown, and the text comes from the kernel rather than "
                                       "from this machine.")
                                        .arg(path));

    if (path != shownFile) return;

    text->setPlainText(cachedText.value(path));
    refreshGutter();
    refreshCurrentLine();
}

void SourceView::setCurrentLine(const QString& file, std::uint32_t line) {
    stoppedInFile = file;
    currentLine = line;

    // A reader who stepped into another module means to see that module, so the window follows.
    if (line != 0 && !file.isEmpty() && file != shownFile) {
        showFile(file);
        return;
    }

    refreshCurrentLine();
    gutter->update();
}

void SourceView::setBreakpoints(const QHash<QString, QHash<std::uint32_t, Marker>>& byFile) {
    markers = byFile;
    gutter->update();
}

int SourceView::gutterWidth() const {
    int digits = 1;
    for (int lines = text->blockCount(); lines >= 10; lines /= 10) ++digits;

    const int digitWidth = text->fontMetrics().horizontalAdvance(QLatin1Char('9'));
    return digitWidth * (digits + GutterPadding + 1);
}

void SourceView::refreshGutter() {
    gutter->setFixedWidth(gutterWidth());
    gutter->update();
}

void SourceView::refreshCurrentLine() {
    QList<QTextEdit::ExtraSelection> selections;

    if (currentLine != 0 && stoppedInFile == shownFile) {
        const QTextBlock block =
            text->document()->findBlockByNumber(static_cast<int>(currentLine) - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection selection;

            // A wash behind the text, not a change of text colour. The text is what is being
            // read, and recolouring it would make the line the design stopped on the hardest
            // one on screen to read.
            selection.format.setBackground(theme->color(ThemeColor::SourceCurrentLine));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = QTextCursor(block);
            selections.append(selection);

            // Brought into view, because a marker nobody can see is not a marker.
            const QTextCursor cursor(block);
            text->setTextCursor(cursor);
            text->ensureCursorVisible();
        }
    }

    text->setExtraSelections(selections);
}

const SourceView::Marker* SourceView::markerAt(std::uint32_t line) const {
    const auto file = markers.constFind(shownFile);
    if (file == markers.constEnd()) return nullptr;

    const auto marker = file->constFind(line);
    return marker == file->constEnd() ? nullptr : &*marker;
}

std::uint32_t SourceView::lineAt(int y) const {
    QTextBlock block = text->firstVisibleBlock();
    int top = static_cast<int>(
        text->blockBoundingGeometry(block).translated(text->contentOffset()).top());

    // Walks the blocks until one covers `y`, and stops as soon as it is past it. Bounded by the
    // document rather than by the gutter's own height: a widget that has not been laid out yet
    // has a height that means nothing, and a hit test that consulted it would answer
    // differently before and after the first paint.
    while (block.isValid()) {
        if (y < top) return 0;

        const int height = static_cast<int>(text->blockBoundingRect(block).height());
        if (y < top + height) return static_cast<std::uint32_t>(block.blockNumber() + 1);

        top += height;
        block = block.next();
    }
    return 0;
}

void SourceView::paintGutter(QPaintEvent* event) {
    QPainter painter(gutter);
    painter.fillRect(event->rect(), theme->color(ThemeColor::SourceGutterBackground));

    QTextBlock block = text->firstVisibleBlock();
    int top = static_cast<int>(
        text->blockBoundingGeometry(block).translated(text->contentOffset()).top());

    const int digitWidth = text->fontMetrics().horizontalAdvance(QLatin1Char('9'));

    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = static_cast<int>(text->blockBoundingRect(block).height());
        const auto line = static_cast<std::uint32_t>(block.blockNumber() + 1);

        if (block.isVisible() && top + height >= event->rect().top()) {
            const Marker* marker = markerAt(line);
            const bool stopped = currentLine == line && stoppedInFile == shownFile;

            if (marker != nullptr) {
                const auto size = static_cast<int>(height * BreakpointSize);
                const QRect dot(digitWidth / 2, top + ((height - size) / 2), size, size);
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme->color(marker->enabled
                                                  ? ThemeColor::SourceBreakpoint
                                                  : ThemeColor::SourceBreakpointDisabled));
                painter.drawEllipse(dot);
            }

            if (stopped) {
                // A triangle pointing at the line, which is the shape every debugger uses and
                // the one that reads as "here" rather than as "selected".
                const auto size = static_cast<int>(height * BreakpointSize);
                const int left = digitWidth / 2;
                const int middle = top + (height / 2);
                const QPolygon arrow({QPoint(left, middle - (size / 2)),
                                      QPoint(left + size, middle),
                                      QPoint(left, middle + (size / 2))});
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme->color(ThemeColor::SourceCurrentMarker));
                painter.drawPolygon(arrow);
            }

            painter.setPen(theme->color(stopped ? ThemeColor::SourceCurrentMarker
                                                : ThemeColor::SourceGutterForeground));
            painter.drawText(QRect(0, top, gutter->width() - digitWidth, height),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(line));
        }

        top += height;
        block = block.next();
    }
}

} // namespace enzan::gui
