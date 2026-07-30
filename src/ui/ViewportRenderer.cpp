#include "ViewportRenderer.h"
#include "IncrementalHighlighter.h"
#include "../engine/PieceTable.h"
#include "../engine/SparseLineIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace {

// Number of lines fetched above the viewport to prime highlighter state
// (a comment or CDATA section opened further up still colours correctly).
constexpr int kContextLines = 10;

constexpr int kWheelLines = 3;

// Extra columns decoded beyond the viewport so small scrolls are free.
constexpr int kColumnSlack = 256;

// Length in bytes of the UTF-8 sequence introduced by lead byte c.
int utf8SeqLen(unsigned char c)
{
    if (c < 0x80)         return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // stray continuation or invalid lead — treat as one byte
}

bool isWordByte(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) || c == '_' || c == '-' || c == ':' || c == '.';
}

struct Palette {
    QColor background;
    QColor gutterBg;
    QColor gutterFg;
    QColor currentLine;
    QColor selection;
    QColor matchHighlight;
    QColor marker;
    QColor caret;
};

Palette lightPalette()
{
    return {QColor(0xFF, 0xFF, 0xFF), QColor(0xF5, 0xF5, 0xF5), QColor(0x90, 0x90, 0x90),
            QColor(0xEE, 0xF5, 0xFF), QColor(0xB4, 0xD5, 0xFE), QColor(0xFF, 0xE9, 0x7F),
            QColor(0xE4, 0xE4, 0xE4), QColor(0x00, 0x00, 0x00)};
}

Palette darkPalette()
{
    return {QColor(0x1E, 0x21, 0x27), QColor(0x24, 0x28, 0x2F), QColor(0x60, 0x6A, 0x76),
            QColor(0x2A, 0x2F, 0x38), QColor(0x2D, 0x4F, 0x76), QColor(0x5A, 0x4A, 0x00),
            QColor(0x32, 0x37, 0x40), QColor(0xE0, 0xE0, 0xE0)};
}

} // namespace

ViewportRenderer::ViewportRenderer(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::IBeamCursor);

    m_highlighter = new IncrementalHighlighter();

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPointSize(10);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    m_vScroll = new QScrollBar(Qt::Vertical, this);
    m_hScroll = new QScrollBar(Qt::Horizontal, this);

    connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int v) {
        const auto line = static_cast<uint64_t>(std::max(0, v));
        if (line == m_firstVisibleLine) return;
        m_firstVisibleLine = line;
        invalidateLines();
        update();
    });
    connect(m_hScroll, &QScrollBar::valueChanged, this, [this](int) { update(); });

    m_caretTimer = new QTimer(this);
    m_caretTimer->setInterval(QApplication::cursorFlashTime() / 2);
    connect(m_caretTimer, &QTimer::timeout, this, [this] {
        m_caretVisible = !m_caretVisible;
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
    m_rangeActive      = false;
    m_rangeStart       = 0;
    m_rangeEnd         = 0;
    m_scrollLineHint   = 0;
    m_firstVisibleLine = 0;
    m_cursorOffset     = 0;
    m_selAnchor        = 0;
    m_selValid         = false;
    m_stickyColumn     = -1;
    clearMatchHighlight();
    invalidateLines();
    updateScrollBars();
    update();
}

void ViewportRenderer::setTabWidth(int spaces)
{
    m_tabWidth = std::max(1, spaces);
    invalidateLines();
    update();
}

void ViewportRenderer::setWordWrap(bool on)
{
    // Word wrap changes visual-line accounting throughout the index; until
    // SparseLineIndex tracks visual lines (TVP-12) the flag is stored only.
    m_wordWrap = on;
    update();
}

void ViewportRenderer::setColumnMarkers(int c1, int c2)
{
    m_colMarker1 = c1;
    m_colMarker2 = c2;
    update();
}

void ViewportRenderer::setReadOnly(bool on) { m_readOnly = on; update(); }

void ViewportRenderer::setEditorFont(const QFont& font)
{
    m_font       = font;
    m_lineHeight = 0; // recomputed on next paint
    invalidateLines();
    updateScrollBars();
    update();
}

void ViewportRenderer::setDarkTheme(bool on)
{
    m_darkTheme = on;
    m_highlighter->setTheme(on ? IncrementalHighlighter::darkTheme()
                              : IncrementalHighlighter::lightTheme());
    update();
}

uint64_t ViewportRenderer::rawDocumentLength() const
{
    return m_table ? m_table->length() : 0;
}

uint64_t ViewportRenderer::documentLength() const
{
    const uint64_t raw = rawDocumentLength();
    return m_rangeActive ? std::min(m_rangeEnd, raw) : raw;
}

uint64_t ViewportRenderer::viewRangeStart() const
{
    return m_rangeActive ? std::min(m_rangeStart, rawDocumentLength()) : 0;
}

uint64_t ViewportRenderer::viewRangeEnd() const
{
    return m_rangeActive ? std::min(m_rangeEnd, rawDocumentLength()) : rawDocumentLength();
}

void ViewportRenderer::setViewRange(uint64_t start, uint64_t end)
{
    const uint64_t raw = rawDocumentLength();
    start = std::min(start, raw);
    end   = std::min(std::max(end, start), raw);

    m_rangeActive = true;
    m_rangeStart  = start;
    m_rangeEnd    = end;

    m_cursorOffset = start;
    m_selAnchor    = start;
    m_selValid     = false;
    m_stickyColumn = -1;
    clearMatchHighlight();

    invalidateLines();
    // Scroll to the first line of the scope.
    if (m_index) {
        m_firstVisibleLine = m_index->offsetToLine(start);
        m_vScroll->setValue(static_cast<int>(std::min<uint64_t>(m_firstVisibleLine, INT32_MAX)));
    }
    updateScrollBars();
    update();
    emit cursorMoved(m_cursorOffset);
}

void ViewportRenderer::clearViewRange()
{
    if (!m_rangeActive) return;
    m_rangeActive = false;
    m_rangeStart  = 0;
    m_rangeEnd    = 0;
    invalidateLines();
    updateScrollBars();
    update();
    emit cursorMoved(m_cursorOffset);
}

// --- Selection ---

uint64_t ViewportRenderer::selectionStart() const
{
    return hasSelection() ? std::min(m_selAnchor, m_cursorOffset) : m_cursorOffset;
}

uint64_t ViewportRenderer::selectionEnd() const
{
    return hasSelection() ? std::max(m_selAnchor, m_cursorOffset) : m_cursorOffset;
}

void ViewportRenderer::clearSelection()
{
    m_selValid  = false;
    m_selAnchor = m_cursorOffset;
    update();
    emit selectionChanged();
}

void ViewportRenderer::selectRange(uint64_t start, uint64_t end)
{
    // A scoped view must not be able to select outside the element it shows.
    const uint64_t lo = viewRangeStart();
    const uint64_t hi = documentLength();
    m_selAnchor    = std::clamp(start, lo, hi);
    m_cursorOffset = std::clamp(end, lo, hi);
    m_selValid     = true;
    ensureCursorVisible();
    update();
    emit cursorMoved(m_cursorOffset);
    emit selectionChanged();
}

QString ViewportRenderer::selectedText() const
{
    if (!m_table || !hasSelection()) return {};
    const uint64_t s = selectionStart();
    const uint64_t e = selectionEnd();
    return QString::fromUtf8(m_table->read(s, e - s).c_str());
}

void ViewportRenderer::setMatchHighlight(uint64_t start, uint64_t length)
{
    m_matchStart  = start;
    m_matchLength = length;
    update();
}

void ViewportRenderer::clearMatchHighlight()
{
    m_matchStart  = 0;
    m_matchLength = 0;
    update();
}

// --- Cursor ---

uint64_t ViewportRenderer::cursorLine() const
{
    return m_index ? m_index->offsetToLine(m_cursorOffset) : 0;
}

int ViewportRenderer::cursorColumn() const
{
    if (!m_index) return 0;
    const VisualLine line = buildLine(cursorLine());
    if (m_cursorOffset <= line.startOffset) return 0;
    const auto within = static_cast<int>(m_cursorOffset - line.startOffset);
    const auto it = std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(), within);
    const int unit = static_cast<int>(it - line.unitToByte.begin());

    std::vector<int> unitToCell;
    expandTabs(line.text, &unitToCell);
    return unitToCell[static_cast<size_t>(std::min<int>(unit, line.text.size()))];
}

void ViewportRenderer::setCursorOffset(uint64_t offset, bool extendSelection)
{
    moveCursor(std::clamp(offset, viewRangeStart(), documentLength()), extendSelection);
}

void ViewportRenderer::moveCursor(uint64_t offset, bool extendSelection)
{
    offset = std::clamp(offset, viewRangeStart(), documentLength());
    if (extendSelection) {
        if (!m_selValid) { m_selAnchor = m_cursorOffset; m_selValid = true; }
    } else {
        m_selValid  = false;
        m_selAnchor = offset;
    }
    m_cursorOffset = offset;
    m_stickyColumn = -1;
    m_caretVisible = true;
    ensureCursorVisible();
    update();
    emit cursorMoved(offset);
    emit selectionChanged();
}

void ViewportRenderer::ensureCursorVisible()
{
    if (!m_index) return;
    const uint64_t line    = cursorLine();
    const auto     visible = static_cast<uint64_t>(std::max(1, visibleLineCount() - 2));

    if (line < m_firstVisibleLine) {
        scrollToLine(line);
    } else if (line >= m_firstVisibleLine + visible) {
        scrollToLine(line - visible + 1);
    }
}

// --- Paint ---

void ViewportRenderer::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const Palette pal = m_darkTheme ? darkPalette() : lightPalette();

    p.setFont(m_font);
    const QFontMetrics fm(m_font);
    m_lineHeight = fm.height();
    m_charWidth  = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    m_ascent     = fm.ascent();

    p.fillRect(rect(), pal.background);

    const int gutter = gutterWidth();
    p.fillRect(0, 0, gutter, height(), pal.gutterBg);

    if (!m_table || !m_index) return;

    rebuildVisibleLines();
    syncScrollRangeIfStale();

    const auto tokens = m_highlighter->tokenise(
        [this] {
            std::vector<QString> texts;
            texts.reserve(m_lines.size());
            for (const auto& l : m_lines) texts.push_back(l.text);
            return texts;
        }(),
        contextLines());

    const int      originX  = textOriginX();
    const uint64_t selStart = selectionStart();
    const uint64_t selEnd   = selectionEnd();
    const bool     haveSel  = hasSelection();
    const uint64_t curLine  = cursorLine();
    const int      clipX    = gutter;

    // Column markers behind the text.
    for (int col : {m_colMarker1, m_colMarker2}) {
        if (col <= 0) continue;
        const int x = originX + col * m_charWidth;
        if (x <= gutter || x > width()) continue;
        p.setPen(pal.marker);
        p.drawLine(x, 0, x, height());
    }

    for (size_t i = 0; i < m_lines.size(); ++i) {
        const VisualLine& line    = m_lines[i];
        const uint64_t    lineNum = m_firstVisibleLine + i;
        const int         top     = static_cast<int>(i) * m_lineHeight;
        const int         baseline = top + m_ascent;
        if (top > height()) break;

        std::vector<int> unitToCell;
        expandTabs(line.text, &unitToCell);

        // Current-line background.
        if (lineNum == curLine && !haveSel) {
            p.fillRect(gutter, top, width() - gutter, m_lineHeight, pal.currentLine);
        }

        // Search-match highlight.
        if (m_matchLength > 0) {
            const uint64_t ms = m_matchStart;
            const uint64_t me = m_matchStart + m_matchLength;
            const uint64_t ls = line.startOffset;
            const uint64_t le = line.startOffset + line.byteLength;
            if (ms < le && me > ls) {
                const int a = unitToCell[static_cast<size_t>(
                    std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(),
                                     static_cast<int>(ms > ls ? ms - ls : 0))
                    - line.unitToByte.begin())];
                const int b = unitToCell[static_cast<size_t>(
                    std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(),
                                     static_cast<int>(std::min(me - ls, line.byteLength)))
                    - line.unitToByte.begin())];
                p.fillRect(originX + a * m_charWidth, top, std::max(1, (b - a) * m_charWidth),
                           m_lineHeight, pal.matchHighlight);
            }
        }

        // Selection background, including the newline when the range spans it.
        if (haveSel) {
            const uint64_t ls = line.startOffset;
            const uint64_t le = line.startOffset + line.byteLength;
            if (selStart <= le && selEnd >= ls) {
                const int a = unitToCell[static_cast<size_t>(
                    std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(),
                                     static_cast<int>(selStart > ls ? selStart - ls : 0))
                    - line.unitToByte.begin())];
                int b;
                if (selEnd > le) {
                    // Selection continues onto the next line — paint to the edge.
                    b = unitToCell.back() + 1;
                } else {
                    b = unitToCell[static_cast<size_t>(
                        std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(),
                                         static_cast<int>(selEnd - ls))
                        - line.unitToByte.begin())];
                }
                if (b > a) {
                    p.fillRect(originX + a * m_charWidth, top, (b - a) * m_charWidth,
                               m_lineHeight, pal.selection);
                }
            }
        }

        // Gutter line number (drawn after backgrounds so it is never covered).
        p.fillRect(0, top, gutter, m_lineHeight, pal.gutterBg);
        p.setPen(pal.gutterFg);
        p.drawText(0, top, gutter - 6, m_lineHeight, Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(lineNum + 1));

        // Tokens, clipped to the text area so long lines do not spill into the gutter.
        p.save();
        p.setClipRect(clipX, top, width() - clipX, m_lineHeight);
        // Only tokens intersecting the visible column window are worth
        // drawing; a long line is mostly off-screen and shaping it is the
        // single most expensive thing this widget does.
        const int firstCol = m_hScroll->value();
        const int lastCol  = firstCol + visibleColumns() + 1;

        if (i < tokens.size() && !tokens[i].empty()) {
            for (const auto& tok : tokens[i]) {
                const int tokStart = unitToCell[static_cast<size_t>(
                    std::min<int>(tok.start, line.text.size()))];
                const int tokEnd = unitToCell[static_cast<size_t>(
                    std::min<int>(tok.start + tok.length, line.text.size()))];
                if (tokEnd < firstCol || tokStart > lastCol) continue;

                p.setPen(m_highlighter->colorFor(tok.kind));
                // Split each token at tabs: tabs advance the cell grid but paint nothing.
                int u = tok.start;
                const int endU = std::min(tok.start + tok.length, static_cast<int>(line.text.size()));
                while (u < endU) {
                    if (line.text[u] == QLatin1Char('\t')) { ++u; continue; }
                    int r = u;
                    while (r < endU && line.text[r] != QLatin1Char('\t')) ++r;
                    p.drawText(originX + unitToCell[static_cast<size_t>(u)] * m_charWidth,
                               baseline, line.text.mid(u, r - u));
                    u = r;
                }
            }
        } else if (!line.text.isEmpty()) {
            p.setPen(m_highlighter->colorFor(Token::Kind::Text));
            p.drawText(originX, baseline, line.text);
        }
        p.restore();

        // Caret.
        if (m_caretVisible && hasFocus() && !m_readOnly && lineNum == curLine) {
            const auto within = static_cast<int>(
                m_cursorOffset > line.startOffset ? m_cursorOffset - line.startOffset : 0);
            const int unit = static_cast<int>(
                std::lower_bound(line.unitToByte.begin(), line.unitToByte.end(), within)
                - line.unitToByte.begin());
            const int x = originX + unitToCell[static_cast<size_t>(
                std::min<int>(unit, line.text.size()))] * m_charWidth;
            p.setPen(pal.caret);
            p.drawLine(x, top, x, top + m_lineHeight - 1);
        }
    }
}

// --- Keyboard ---

void ViewportRenderer::keyPressEvent(QKeyEvent* e)
{
    if (!m_table || !m_index) { QWidget::keyPressEvent(e); return; }

    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);
    const bool ctrl  = e->modifiers().testFlag(Qt::ControlModifier);
    const auto docLen = documentLength();

    // Vertical movement keeps the "sticky" column so travelling through short
    // lines and back out preserves the original column.
    auto moveVertical = [&](int64_t delta) {
        const int col = (m_stickyColumn >= 0) ? m_stickyColumn : cursorColumn();
        const uint64_t line   = cursorLine();
        int64_t        target = static_cast<int64_t>(line) + delta;
        if (target < 0) target = 0;

        const uint64_t lineStart = m_index->lineToOffset(static_cast<uint64_t>(target));
        if (lineStart >= docLen && delta > 0) {
            moveCursor(docLen, shift);
            m_stickyColumn = col;
            return;
        }
        const VisualLine tl = buildLine(static_cast<uint64_t>(target));
        std::vector<int> unitToCell;
        expandTabs(tl.text, &unitToCell);

        // Clamp the sticky column to the end of the target line.
        int unit = static_cast<int>(unitToCell.size()) - 1;
        for (size_t u = 0; u < unitToCell.size(); ++u) {
            if (unitToCell[u] >= col) { unit = static_cast<int>(u); break; }
        }
        moveCursor(tl.startOffset + static_cast<uint64_t>(tl.unitToByte[static_cast<size_t>(unit)]), shift);
        m_stickyColumn = col;
    };

    auto lineStartOffset = [&] { return m_index->lineToOffset(cursorLine()); };
    auto lineEnd         = [&] { return m_index->lineEndOffset(cursorLine()); };

    switch (e->key()) {
    case Qt::Key_Left:
        if (hasSelection() && !shift) moveCursor(selectionStart(), false);
        else moveCursor(prevCharOffset(m_cursorOffset), shift);
        return;
    case Qt::Key_Right:
        if (hasSelection() && !shift) moveCursor(selectionEnd(), false);
        else moveCursor(nextCharOffset(m_cursorOffset), shift);
        return;
    case Qt::Key_Up:
        if (ctrl) { m_vScroll->setValue(m_vScroll->value() - 1); return; }
        moveVertical(-1);
        return;
    case Qt::Key_Down:
        if (ctrl) { m_vScroll->setValue(m_vScroll->value() + 1); return; }
        moveVertical(1);
        return;
    case Qt::Key_Home:
        moveCursor(ctrl ? 0 : lineStartOffset(), shift);
        return;
    case Qt::Key_End:
        moveCursor(ctrl ? docLen : lineEnd(), shift);
        return;
    case Qt::Key_PageUp:
        moveVertical(-std::max(1, visibleLineCount() - 2));
        return;
    case Qt::Key_PageDown:
        moveVertical(std::max(1, visibleLineCount() - 2));
        return;

    case Qt::Key_Backspace: backspace();    return;
    case Qt::Key_Delete:    deleteForward(); return;

    case Qt::Key_Return:
    case Qt::Key_Enter:
        insertText(QStringLiteral("\n"));
        return;
    case Qt::Key_Tab:
        if (!ctrl) { insertText(QStringLiteral("\t")); return; }
        break;

    case Qt::Key_A: if (ctrl) { selectAll(); return; } break;
    case Qt::Key_C: if (ctrl) { copy();      return; } break;
    case Qt::Key_X: if (ctrl) { cut();       return; } break;
    case Qt::Key_V: if (ctrl) { paste();     return; } break;
    default: break;
    }

    // Printable input. Ctrl/Alt chords are left to the menu shortcuts.
    if (!e->text().isEmpty() && !ctrl && !e->modifiers().testFlag(Qt::AltModifier)) {
        const QChar c = e->text().at(0);
        if (c.isPrint() || c == QLatin1Char('\t')) { insertText(e->text()); return; }
    }

    QWidget::keyPressEvent(e);
}

// --- Editing ---

void ViewportRenderer::applyEdit(uint64_t offset, int64_t delta)
{
    // Keep a scoped view covering the same element after its content grows or
    // shrinks; an edit before the scope shifts both ends.
    if (m_rangeActive && delta != 0) {
        if (offset < m_rangeStart)
            m_rangeStart = static_cast<uint64_t>(std::max<int64_t>(
                0, static_cast<int64_t>(m_rangeStart) + delta));
        if (offset <= m_rangeEnd)
            m_rangeEnd = static_cast<uint64_t>(std::max<int64_t>(
                static_cast<int64_t>(m_rangeStart),
                static_cast<int64_t>(m_rangeEnd) + delta));
    }

    if (m_index) m_index->invalidateFrom(offset);
    invalidateLines();
    updateScrollBars();
    m_stickyColumn = -1;
    m_caretVisible = true;
    ensureCursorVisible();
    update();
    emit documentEdited(offset);
    emit cursorMoved(m_cursorOffset);
    emit selectionChanged();
}

void ViewportRenderer::insertText(const QString& text)
{
    if (!m_table || m_readOnly || text.isEmpty()) return;

    const QByteArray utf8 = text.toUtf8();
    uint64_t at = m_cursorOffset;
    const uint64_t lengthBefore = rawDocumentLength();

    if (hasSelection()) {
        // Replacing a selection must undo in one step.
        const uint64_t s = selectionStart();
        const uint64_t e = selectionEnd();
        m_table->replace(s, e - s, std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
        at = s;
        m_selValid = false;
    } else {
        m_table->insert(at, std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
    }

    m_cursorOffset = at + static_cast<uint64_t>(utf8.size());
    m_selAnchor    = m_cursorOffset;
    applyEdit(at, static_cast<int64_t>(rawDocumentLength()) - static_cast<int64_t>(lengthBefore));
}

void ViewportRenderer::deleteSelection()
{
    if (!m_table || m_readOnly || !hasSelection()) return;
    const uint64_t s = selectionStart();
    const uint64_t e = selectionEnd();
    m_table->remove(s, e - s);
    m_cursorOffset = s;
    m_selAnchor    = s;
    m_selValid     = false;
    applyEdit(s, -static_cast<int64_t>(e - s));
}

void ViewportRenderer::backspace()
{
    if (!m_table || m_readOnly) return;
    if (hasSelection()) { deleteSelection(); return; }
    if (m_cursorOffset == 0) return;

    uint64_t from = prevCharOffset(m_cursorOffset);
    // A CRLF pair deletes as a single unit.
    if (m_cursorOffset - from == 1 && from > 0
        && m_table->read(from, 1) == "\n" && m_table->read(from - 1, 1) == "\r") {
        --from;
    }
    const uint64_t removed = m_cursorOffset - from;
    m_table->remove(from, removed);
    m_cursorOffset = from;
    m_selAnchor    = from;
    applyEdit(from, -static_cast<int64_t>(removed));
}

void ViewportRenderer::deleteForward()
{
    if (!m_table || m_readOnly) return;
    if (hasSelection()) { deleteSelection(); return; }

    const uint64_t docLen = documentLength();
    if (m_cursorOffset >= docLen) return;

    uint64_t to = nextCharOffset(m_cursorOffset);
    if (m_table->read(m_cursorOffset, 1) == "\r" && to < docLen && m_table->read(to, 1) == "\n")
        ++to;
    m_table->remove(m_cursorOffset, to - m_cursorOffset);
    applyEdit(m_cursorOffset, -static_cast<int64_t>(to - m_cursorOffset));
}

void ViewportRenderer::cut()
{
    if (!hasSelection()) return;
    copy();
    if (!m_readOnly) deleteSelection();
}

void ViewportRenderer::copy()
{
    if (!hasSelection()) return;
    QApplication::clipboard()->setText(selectedText());
}

void ViewportRenderer::paste()
{
    if (m_readOnly) return;
    const QString text = QApplication::clipboard()->text();
    if (!text.isEmpty()) insertText(text);
}

void ViewportRenderer::selectAll()
{
    if (!m_table) return;
    selectRange(viewRangeStart(), documentLength());
}

void ViewportRenderer::undo()
{
    if (!m_table || !m_table->canUndo()) return;
    uint64_t cursor = m_cursorOffset;
    m_table->undo(&cursor);
    m_cursorOffset = std::min(cursor, documentLength());
    m_selAnchor    = m_cursorOffset;
    m_selValid     = false;
    // The piece list changed wholesale, so every checkpoint may be stale — and
    // a scoped range can no longer be trusted to still bound the same element.
    clearViewRange();
    applyEdit(0, 0);
}

void ViewportRenderer::redo()
{
    if (!m_table || !m_table->canRedo()) return;
    uint64_t cursor = m_cursorOffset;
    m_table->redo(&cursor);
    m_cursorOffset = std::min(cursor, documentLength());
    m_selAnchor    = m_cursorOffset;
    m_selValid     = false;
    clearViewRange();
    applyEdit(0, 0);
}

// --- Mouse ---

void ViewportRenderer::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    if (e->button() != Qt::LeftButton || !m_table || !m_index) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_selecting = true;
    moveCursor(offsetAtPoint(e->pos()), e->modifiers().testFlag(Qt::ShiftModifier));
}

void ViewportRenderer::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_selecting || !m_table || !m_index) { QWidget::mouseMoveEvent(e); return; }

    // Auto-scroll when dragging past the top or bottom edge.
    if (e->pos().y() < 0)             m_vScroll->setValue(m_vScroll->value() - 1);
    else if (e->pos().y() > height()) m_vScroll->setValue(m_vScroll->value() + 1);

    moveCursor(offsetAtPoint(e->pos()), true);
}

void ViewportRenderer::mouseReleaseEvent(QMouseEvent* e)
{
    m_selecting = false;
    QWidget::mouseReleaseEvent(e);
}

void ViewportRenderer::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !m_table || !m_index) {
        QWidget::mouseDoubleClickEvent(e);
        return;
    }
    uint64_t start = 0, end = 0;
    wordBoundsAt(offsetAtPoint(e->pos()), &start, &end);
    if (end > start) selectRange(start, end);
}

void ViewportRenderer::wheelEvent(QWheelEvent* e)
{
    // The range may predate anything the index has since learned; without this
    // the wheel is inert on a document that has not been painted yet.
    syncScrollRangeIfStale();

    const int steps = e->angleDelta().y() / 120;
    if (steps != 0) {
        m_vScroll->setValue(m_vScroll->value() - steps * kWheelLines);
        e->accept();
        return;
    }
    QWidget::wheelEvent(e);
}

void ViewportRenderer::resizeEvent(QResizeEvent*)
{
    const int sbW = m_vScroll->sizeHint().width();
    const int sbH = m_hScroll->sizeHint().height();
    m_vScroll->setGeometry(width() - sbW, 0, sbW, height() - sbH);
    m_hScroll->setGeometry(0, height() - sbH, width() - sbW, sbH);
    invalidateLines();
    updateScrollBars();
    update();
}

void ViewportRenderer::focusInEvent(QFocusEvent* e)
{
    m_caretVisible = true;
    m_caretTimer->start();
    QWidget::focusInEvent(e);
}

void ViewportRenderer::focusOutEvent(QFocusEvent* e)
{
    m_caretTimer->stop();
    m_caretVisible = false;
    update();
    QWidget::focusOutEvent(e);
}

// --- Line construction ---

int ViewportRenderer::visibleLineCount() const
{
    if (m_lineHeight <= 0) {
        const QFontMetrics fm(m_font);
        const int h = std::max(1, fm.height());
        return (height() / h) + 2;
    }
    return (height() / m_lineHeight) + 2;
}

void ViewportRenderer::invalidateLines()
{
    m_linesValid = false;
}

ViewportRenderer::VisualLine ViewportRenderer::decodeLineAt(uint64_t start,
                                                            int cellBudget) const
{
    VisualLine line;
    line.startOffset = start;

    if (!m_table) {
        line.unitToByte.push_back(0);
        return line;
    }

    const uint64_t docLen = documentLength(); // scope end when scoped

    // A cell is at least one byte, so budgeting in bytes never under-reads; the
    // x4 covers the worst case of every cell being a 4-byte UTF-8 sequence.
    const size_t maxBytes = static_cast<size_t>(std::max(1, cellBudget)) * 4 + 64;

    std::string raw;
    uint64_t    pos        = start;
    bool        sawNewline = false;
    while (pos < docLen && raw.size() < maxBytes) {
        // Read straight into the accumulator: read() would hand back a fresh
        // std::string per 8 KB, and this runs for every visible line of every
        // frame.
        const size_t want = static_cast<size_t>(std::min<uint64_t>(8192, docLen - pos));
        const size_t base = raw.size();
        raw.resize(base + want);
        const size_t got = m_table->readInto(pos, raw.data() + base, want);
        raw.resize(base + got);
        if (got == 0) break;

        const size_t nl = raw.find('\n', base);
        if (nl != std::string::npos) {
            raw.resize(nl);
            sawNewline = true;
            break;
        }
        pos += got;
    }

    // Ran out of budget before finding the newline: this is a long line and we
    // only materialise the slice the viewport can actually show.
    if (!sawNewline && raw.size() >= maxBytes) {
        raw.resize(maxBytes);
        line.clipped = true;
    }

    line.byteLength = raw.size();
    if (!raw.empty() && raw.back() == '\r') {
        // A trailing CR belongs to the line terminator, not the content.
        raw.pop_back();
    }

    // Decode UTF-8 while recording where each UTF-16 unit started.
    line.text.reserve(static_cast<int>(raw.size()));
    line.unitToByte.reserve(raw.size() + 1);
    for (size_t b = 0; b < raw.size();) {
        // ASCII, which is nearly all of any XML document, converts one run at a
        // time: a QString per character costs ~16× as much, and this is the
        // per-frame path that the 60 fps target lives on. Below 0x80 Latin-1 and
        // UTF-8 agree, and each byte is exactly one UTF-16 unit.
        size_t e = b;
        while (e < raw.size() && static_cast<unsigned char>(raw[e]) < 0x80) ++e;
        if (e > b) {
            line.text += QString::fromLatin1(raw.data() + b, static_cast<qsizetype>(e - b));
            for (size_t k = b; k < e; ++k)
                line.unitToByte.push_back(static_cast<int>(k));
            b = e;
            continue;
        }

        int len = utf8SeqLen(static_cast<unsigned char>(raw[b]));
        if (b + static_cast<size_t>(len) > raw.size()) len = 1;
        QString ch = QString::fromUtf8(raw.data() + b, len);
        if (ch.isEmpty()) ch = QChar(QChar::ReplacementCharacter);
        for (int k = 0; k < ch.size(); ++k)
            line.unitToByte.push_back(static_cast<int>(b));
        line.text += ch;
        b += static_cast<size_t>(len);
    }
    line.unitToByte.push_back(static_cast<int>(line.byteLength));
    return line;
}

void ViewportRenderer::rebuildVisibleLines()
{
    if (m_linesValid) return;
    m_lines.clear();
    if (!m_table || !m_index) { m_linesValid = true; return; }

    const int      count    = visibleLineCount();
    const int      budget   = lineCellBudget();
    const uint64_t docLen   = documentLength();
    const uint64_t rangeTop = viewRangeStart();
    const uint64_t lastLine = m_index->offsetToLine(docLen);
    m_lines.reserve(static_cast<size_t>(count));

    // Walk by line number rather than by "where the previous read stopped". A
    // clipped line is still a single line, so its remainder must not be shown
    // as extra rows with invented line numbers.
    const uint64_t firstScopeLine = m_rangeActive ? m_index->offsetToLine(rangeTop) : 0;

    for (int i = 0; i < count; ++i) {
        const uint64_t lineNum = m_firstVisibleLine + static_cast<uint64_t>(i);
        if (lineNum > lastLine) break;
        if (lineNum < firstScopeLine) continue;

        uint64_t start = m_index->lineToOffset(lineNum);
        if (start > docLen) break;
        // A scoped view starts mid-line when the element does.
        start = std::max(start, rangeTop);
        if (start > docLen) break;

        m_lines.push_back(decodeLineAt(start, budget));
    }

    m_linesValid = true;
}

ViewportRenderer::VisualLine ViewportRenderer::buildLine(uint64_t lineNumber) const
{
    // Serve from the visible cache when possible.
    if (m_linesValid && lineNumber >= m_firstVisibleLine) {
        const auto i = static_cast<size_t>(lineNumber - m_firstVisibleLine);
        if (i < m_lines.size()) return m_lines[i];
    }

    VisualLine line;
    if (!m_table || !m_index) {
        line.unitToByte.push_back(0);
        return line;
    }
    return decodeLineAt(m_index->lineToOffset(lineNumber), lineCellBudget());
}

std::vector<QString> ViewportRenderer::contextLines() const
{
    std::vector<QString> out;
    if (!m_index || m_firstVisibleLine == 0) return out;

    const uint64_t from = (m_firstVisibleLine > static_cast<uint64_t>(kContextLines))
        ? m_firstVisibleLine - kContextLines : 0;
    out.reserve(static_cast<size_t>(m_firstVisibleLine - from));
    for (uint64_t l = from; l < m_firstVisibleLine; ++l)
        out.push_back(buildLine(l).text);
    return out;
}

// --- Geometry ---

int ViewportRenderer::visibleColumns() const
{
    int cw = m_charWidth;
    if (cw <= 0) {
        const QFontMetrics fm(m_font);
        cw = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    }
    return std::max(16, (width() - gutterWidth()) / cw);
}

int ViewportRenderer::lineCellBudget() const
{
    // Only what the viewport can show, plus slack so a small horizontal scroll
    // does not force a re-read. Without this a minified document would decode
    // and tokenise ~65 000 columns per row to display ~125 of them.
    return m_hScroll->value() + visibleColumns() + kColumnSlack;
}

int ViewportRenderer::gutterWidth() const
{
    const QFontMetrics fm(m_font);
    const uint64_t last = m_index ? m_index->estimatedLineCount() : 1;
    const int digits = std::max(4, static_cast<int>(QString::number(last + 1).size()));
    return fm.horizontalAdvance(QString(digits, QLatin1Char('9'))) + 12;
}

int ViewportRenderer::textOriginX() const
{
    return gutterWidth() - m_hScroll->value() * std::max(1, m_charWidth);
}

QString ViewportRenderer::expandTabs(const QString& s, std::vector<int>* unitToCell) const
{
    QString out;
    out.reserve(s.size() + 8);
    if (unitToCell) { unitToCell->clear(); unitToCell->reserve(static_cast<size_t>(s.size()) + 1); }

    int cell = 0;
    for (int i = 0; i < s.size(); ++i) {
        if (unitToCell) unitToCell->push_back(cell);
        if (s[i] == QLatin1Char('\t')) {
            const int advance = m_tabWidth - (cell % m_tabWidth);
            out.append(QString(advance, QLatin1Char(' ')));
            cell += advance;
        } else {
            out.append(s[i]);
            ++cell;
        }
    }
    if (unitToCell) unitToCell->push_back(cell);
    return out;
}

uint64_t ViewportRenderer::offsetAtPoint(const QPoint& pos) const
{
    if (!m_table || !m_index) return 0;

    const_cast<ViewportRenderer*>(this)->rebuildVisibleLines();
    if (m_lines.empty()) return 0;

    const int lh  = std::max(1, m_lineHeight);
    int       row = pos.y() / lh;
    row = std::clamp(row, 0, static_cast<int>(m_lines.size()) - 1);

    const VisualLine& line = m_lines[static_cast<size_t>(row)];
    std::vector<int> unitToCell;
    expandTabs(line.text, &unitToCell);

    const int cw = std::max(1, m_charWidth);
    // Round to the nearest cell boundary so clicking a glyph's right half puts
    // the caret after it.
    const int rel  = pos.x() - textOriginX();
    const int cell = std::max(0, (rel + cw / 2) / cw);

    int unit = static_cast<int>(unitToCell.size()) - 1;
    for (size_t u = 0; u < unitToCell.size(); ++u) {
        if (unitToCell[u] >= cell) { unit = static_cast<int>(u); break; }
    }
    return line.startOffset + static_cast<uint64_t>(line.unitToByte[static_cast<size_t>(unit)]);
}

void ViewportRenderer::wordBoundsAt(uint64_t offset, uint64_t* start, uint64_t* end) const
{
    *start = offset;
    *end   = offset;
    if (!m_table) return;

    const uint64_t docLen = documentLength();
    // Walk out from the click in both directions over word bytes.
    uint64_t s = offset;
    while (s > 0) {
        const std::string b = m_table->read(s - 1, 1);
        if (b.empty() || !isWordByte(b[0])) break;
        --s;
    }
    uint64_t e = offset;
    while (e < docLen) {
        const std::string b = m_table->read(e, 1);
        if (b.empty() || !isWordByte(b[0])) break;
        ++e;
    }
    if (e == s && e < docLen) ++e; // click landed on punctuation — select it
    *start = s;
    *end   = e;
}

uint64_t ViewportRenderer::prevCharOffset(uint64_t offset) const
{
    if (!m_table || offset == 0) return 0;
    uint64_t p = offset - 1;
    while (p > 0) {
        const std::string b = m_table->read(p, 1);
        if (b.empty()) break;
        if ((static_cast<unsigned char>(b[0]) & 0xC0) != 0x80) break; // not a continuation byte
        --p;
    }
    return p;
}

uint64_t ViewportRenderer::nextCharOffset(uint64_t offset) const
{
    if (!m_table) return offset;
    const uint64_t docLen = documentLength();
    if (offset >= docLen) return docLen;

    const std::string b = m_table->read(offset, 1);
    if (b.empty()) return docLen;
    return std::min(docLen, offset + static_cast<uint64_t>(
        utf8SeqLen(static_cast<unsigned char>(b[0]))));
}

// --- Scrolling ---

void ViewportRenderer::scrollToLine(uint64_t line)
{
    if (line == m_firstVisibleLine) return;
    m_firstVisibleLine = line;
    invalidateLines();
    // setValue re-enters the valueChanged lambda, which is a no-op now that
    // m_firstVisibleLine already matches.
    m_vScroll->setValue(static_cast<int>(std::min<uint64_t>(line, INT32_MAX)));
    update();
}

void ViewportRenderer::refreshScrollBars()
{
    m_scrollLineHint = m_index ? m_index->estimatedLineCount() : 0;
    updateScrollBars();
    update();
}

// The index only learns the document's line count as it is queried, so a range
// computed while it was merely attached reports one line and the scroll bar has
// nothing to travel over. Keyboard navigation hid this because scrollToLine()
// moves the view directly, whereas the wheel drives the scroll bar.
void ViewportRenderer::syncScrollRangeIfStale()
{
    if (!m_index) return;
    const uint64_t known = m_index->estimatedLineCount();
    if (known == m_scrollLineHint) return;
    m_scrollLineHint = known;
    updateScrollBars();
}

void ViewportRenderer::updateScrollBars()
{
    if (!m_index) {
        m_vScroll->setRange(0, 0);
        m_hScroll->setRange(0, 0);
        return;
    }

    const int visible = std::max(1, visibleLineCount() - 2);

    // The scroll bar works in absolute line numbers throughout, so a scoped
    // view simply narrows its range rather than needing an index remap.
    uint64_t firstLine = 0;
    uint64_t lastLine  = 0;
    if (m_rangeActive) {
        firstLine = m_index->offsetToLine(viewRangeStart());
        lastLine  = m_index->offsetToLine(documentLength());
    } else {
        const uint64_t lines = m_index->estimatedLineCount();
        lastLine = (lines > 0) ? lines - 1 : 0;
    }
    if (lastLine < firstLine) lastLine = firstLine;

    const uint64_t span    = lastLine - firstLine + 1;
    const uint64_t maxTop  = firstLine
        + (span > static_cast<uint64_t>(visible) ? span - static_cast<uint64_t>(visible) : 0);

    m_vScroll->setRange(static_cast<int>(std::min<uint64_t>(firstLine, INT32_MAX)),
                        static_cast<int>(std::min<uint64_t>(maxTop, INT32_MAX)));
    m_vScroll->setPageStep(visible);
    m_vScroll->setSingleStep(1);

    // Horizontal range from the widest currently visible line.
    int widest = 0;
    for (const auto& l : m_lines) {
        std::vector<int> unitToCell;
        expandTabs(l.text, &unitToCell);
        widest = std::max(widest, unitToCell.back());
    }
    const int cols = std::max(0, width() - gutterWidth()) / std::max(1, m_charWidth);
    m_hScroll->setRange(0, std::max(0, widest - cols + 4));
    m_hScroll->setPageStep(std::max(1, cols));
}
