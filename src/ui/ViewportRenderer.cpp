#include "ViewportRenderer.h"
#include "IncrementalHighlighter.h"
#include "../engine/PieceTable.h"
#include "../engine/SparseLineIndex.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

ViewportRenderer::ViewportRenderer(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_highlighter = new IncrementalHighlighter();

    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_hScroll = new QScrollBar(Qt::Horizontal, this);

    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int v) {
        m_firstVisibleLine = static_cast<uint64_t>(v);
        update();
    });
}

ViewportRenderer::~ViewportRenderer()
{
    delete m_highlighter;
}

void ViewportRenderer::setDocument(PieceTable* table, SparseLineIndex* index)
{
    m_table            = table;
    m_index            = index;
    m_firstVisibleLine = 0;
    m_cursorOffset     = 0;
    updateScrollBars();
    update();
}

void ViewportRenderer::setCursorOffset(uint64_t offset)
{
    m_cursorOffset = offset;
    if (m_index) scrollToLine(m_index->offsetToLine(offset));
    update();
    emit cursorMoved(offset);
}

void ViewportRenderer::setTabWidth(int spaces)   { m_tabWidth = spaces; update(); }
void ViewportRenderer::setWordWrap(bool on)       { m_wordWrap = on; update(); }
void ViewportRenderer::setColumnMarkers(int c1, int c2) { m_colMarker1 = c1; m_colMarker2 = c2; update(); }

// --- Paint ---

void ViewportRenderer::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::white);

    if (!m_table || !m_index) return;

    const QFont mono("Monospace", 10);
    p.setFont(mono);
    const QFontMetrics fm(mono);
    m_lineHeight = fm.height();

    const int gutter = gutterWidth();
    const auto lines = fetchVisibleLines();

    // Highlight context (10 lines above viewport)
    const std::vector<QString> context; // TODO: fetch context lines
    const auto tokens = m_highlighter->tokenise(lines, context);

    int y = m_lineHeight;
    for (size_t i = 0; i < lines.size(); ++i) {
        const uint64_t lineNum = m_firstVisibleLine + i;

        // Gutter
        p.setPen(Qt::gray);
        p.drawText(0, y, gutter - 4, m_lineHeight, Qt::AlignRight,
                   QString::number(lineNum + 1));

        // Line background for current line
        const uint64_t lineOffset = m_index->lineToOffset(lineNum);
        if (lineOffset <= m_cursorOffset &&
            m_cursorOffset < lineOffset + static_cast<uint64_t>(lines[i].size() + 1)) {
            p.fillRect(gutter, y - m_lineHeight + 2, width() - gutter, m_lineHeight,
                       QColor(0xEE, 0xF5, 0xFF));
        }

        // Tokens
        int x = gutter;
        if (i < tokens.size() && !tokens[i].empty()) {
            for (const auto& tok : tokens[i]) {
                const QString part = lines[i].mid(tok.start, tok.length);
                p.setPen(m_highlighter->colorFor(tok.kind));
                p.drawText(x, y, part);
                x += fm.horizontalAdvance(part);
            }
        } else {
            p.setPen(Qt::black);
            p.drawText(gutter, y, lines[i]);
        }

        y += m_lineHeight;
    }

    // Column markers
    auto drawMarker = [&](int col) {
        if (col <= 0) return;
        const int x = gutter + fm.horizontalAdvance(QString(col, 'x'));
        p.setPen(QColor(0xCC, 0xCC, 0xCC));
        p.drawLine(x, 0, x, height());
    };
    drawMarker(m_colMarker1);
    drawMarker(m_colMarker2);
}

// --- Input ---

void ViewportRenderer::keyPressEvent(QKeyEvent* e)
{
    // TODO: implement cursor movement per TVP-06, selection per TVP-07,
    // clipboard per TVP-08, and insert/delete edits via PieceTable.
    QWidget::keyPressEvent(e);
}

void ViewportRenderer::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    // TODO: hit-test line/column from e->pos(), update m_cursorOffset
    QWidget::mousePressEvent(e);
}

void ViewportRenderer::mouseMoveEvent(QMouseEvent* e)
{
    // TODO: extend selection
    QWidget::mouseMoveEvent(e);
}

void ViewportRenderer::wheelEvent(QWheelEvent* e)
{
    m_vScroll->event(e);
}

void ViewportRenderer::resizeEvent(QResizeEvent*)
{
    const int sbW = m_vScroll->sizeHint().width();
    const int sbH = m_hScroll->sizeHint().height();
    m_vScroll->setGeometry(width() - sbW, 0, sbW, height() - sbH);
    m_hScroll->setGeometry(0, height() - sbH, width() - sbW, sbH);
    update();
}

// --- Private ---

int ViewportRenderer::visibleLineCount() const
{
    if (m_lineHeight <= 0) return 40;
    return (height() / m_lineHeight) + 2;
}

std::vector<QString> ViewportRenderer::fetchVisibleLines() const
{
    std::vector<QString> lines;
    if (!m_table || !m_index) return lines;

    const int count = visibleLineCount();
    lines.reserve(static_cast<size_t>(count));

    const uint64_t startOffset = m_index->lineToOffset(m_firstVisibleLine);
    PieceTable::Iterator it = m_table->iteratorAt(startOffset);

    QString current;
    bool done = false;
    while (!it.atEnd() && !done) {
        const std::string_view chunk = it.nextChunk();
        for (size_t i = 0; i < chunk.size() && !done; ++i) {
            if (chunk[i] == '\n') {
                lines.push_back(std::move(current));
                current.clear();
                if (static_cast<int>(lines.size()) >= count)
                    done = true;
            } else {
                current += QLatin1Char(chunk[i]);
            }
        }
    }
    if (static_cast<int>(lines.size()) < count)
        lines.push_back(std::move(current));

    return lines;
}

void ViewportRenderer::scrollToLine(uint64_t line)
{
    const uint64_t half = static_cast<uint64_t>(visibleLineCount() / 2);
    m_firstVisibleLine  = (line > half) ? line - half : 0;
    m_vScroll->setValue(static_cast<int>(m_firstVisibleLine));
    update();
}

void ViewportRenderer::updateScrollBars()
{
    if (!m_index) return;
    m_vScroll->setRange(0, static_cast<int>(m_index->estimatedLineCount()));
    m_vScroll->setPageStep(visibleLineCount());
}

int ViewportRenderer::gutterWidth() const
{
    const QFontMetrics fm(font());
    return fm.horizontalAdvance("99999") + 8;
}
