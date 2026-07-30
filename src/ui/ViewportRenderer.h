#pragma once

#include <QFont>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

class PieceTable;
class SparseLineIndex;
class IncrementalHighlighter;
class QScrollBar;
class QTimer;

// Custom QWidget that paints only the visible lines of the document (~40–80).
// Cursor position is stored as a byte offset, not (line, col), so it survives
// edits. Emits cursorMoved() on every cursor movement for the tree view and
// breadcrumb, and documentEdited() whenever the PieceTable is mutated.
class ViewportRenderer : public QWidget {
    Q_OBJECT
public:
    explicit ViewportRenderer(QWidget* parent = nullptr);
    ~ViewportRenderer() override;

    void setDocument(PieceTable* table, SparseLineIndex* index);

    // --- Scoped view ---
    //
    // Restricts rendering, navigation and the cursor to [start, end) so a single
    // element can be examined without the rest of the document. Editing still
    // targets real document offsets, so saving is unaffected; the range follows
    // edits made inside it.
    void     setViewRange(uint64_t start, uint64_t end);
    void     clearViewRange();
    bool     hasViewRange()    const { return m_rangeActive; }
    uint64_t viewRangeStart()  const;
    uint64_t viewRangeEnd()    const;

    uint64_t cursorOffset() const { return m_cursorOffset; }
    // Topmost rendered line. Exposed so tests can assert that scrolling
    // actually moved the view.
    uint64_t firstVisibleLine() const { return m_firstVisibleLine; }
    int      tabWidth()     const { return m_tabWidth; }
    bool     wordWrap()     const { return m_wordWrap; }
    bool     isReadOnly()   const { return m_readOnly; }

    void setCursorOffset(uint64_t offset, bool extendSelection = false);
    void setTabWidth(int spaces);
    void setWordWrap(bool on);
    void setColumnMarkers(int col1, int col2); // 0 to disable
    void setReadOnly(bool on);
    void setEditorFont(const QFont& font);
    void setDarkTheme(bool on);

    // Selection, as an ordered byte range. Empty when there is no selection.
    bool     hasSelection() const { return m_selAnchor != m_cursorOffset && m_selValid; }
    uint64_t selectionStart() const;
    uint64_t selectionEnd()   const;
    void     clearSelection();
    void     selectRange(uint64_t start, uint64_t end);
    QString  selectedText() const;

    // Highlight a range without moving the selection (used for search matches).
    void setMatchHighlight(uint64_t start, uint64_t length);
    void clearMatchHighlight();

    // Line the cursor is on, 0-based.
    uint64_t cursorLine() const;
    // Visual column of the cursor, 0-based, with tabs expanded.
    int cursorColumn() const;

    void ensureCursorVisible();

    // Recomputes the scroll bar ranges. Call after the line index has learned
    // more about the document, otherwise the vertical range can stay stuck at
    // the estimate taken when the document was attached.
    void refreshScrollBars();

    // --- Editing (no-ops when read-only) ---
    void insertText(const QString& text);
    void deleteSelection();
    void backspace();
    void deleteForward();
    void cut();
    void copy();
    void paste();
    void selectAll();
    void undo();
    void redo();

signals:
    void cursorMoved(uint64_t byteOffset);
    // Emitted after any mutation; offset is the earliest byte changed.
    void documentEdited(uint64_t offset);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    // A visible line, decoded for painting with a byte↔column mapping so that
    // cursor offsets stay exact for multi-byte UTF-8 and tabs.
    struct VisualLine {
        uint64_t         startOffset = 0; // document offset of the first byte
        uint64_t         byteLength  = 0; // bytes decoded, excluding the newline
        QString          text;            // decoded, tabs left as '\t'
        std::vector<int> unitToByte;       // size text.size()+1
        // Set when the line ran past the decode budget rather than reaching a
        // newline. Minified XML is one enormous line, so only the horizontally
        // visible slice is ever materialised.
        bool             clipped     = false;
    };

    int  visibleLineCount() const;
    // Columns that fit across the text area.
    int  visibleColumns() const;
    // Cells a line must be decoded to in order to cover the current horizontal
    // scroll position, plus a margin so small scrolls do not force a rebuild.
    int  lineCellBudget() const;
    // Reads and decodes one line starting at `start`, stopping at the newline or
    // once `cellBudget` cells have been produced.
    VisualLine decodeLineAt(uint64_t start, int cellBudget) const;
    void rebuildVisibleLines();
    void invalidateLines();

    // Lines above the viewport that establish highlighter state.
    std::vector<QString> contextLines() const;

    VisualLine buildLine(uint64_t lineNumber) const;
    // The cached line containing `offset`, or nullptr when it is off-screen.
    const VisualLine* lineForOffset(uint64_t offset) const;

    void scrollToLine(uint64_t line);
    void updateScrollBars();
    // Rebuilds the scroll range if the index has learned more lines since it
    // was last computed. Cheap enough to call before every scroll.
    void syncScrollRangeIfStale();

    int gutterWidth() const;
    int textOriginX() const; // gutter width minus horizontal scroll

    // Pixel x of a UTF-16 unit index within a line, tabs expanded.
    int  xForUnit(const VisualLine& line, int unit) const;
    // Nearest UTF-16 unit index for a pixel x within a line.
    int  unitForX(const VisualLine& line, int x) const;
    // Expand tabs so painting and hit-testing agree on cell positions.
    QString expandTabs(const QString& s, std::vector<int>* unitToCell = nullptr) const;

    uint64_t offsetAtPoint(const QPoint& pos) const;
    // Byte offset of the start of the word around `offset`, and its end.
    void wordBoundsAt(uint64_t offset, uint64_t* start, uint64_t* end) const;

    void moveCursor(uint64_t offset, bool extendSelection);
    // Shared post-mutation bookkeeping. `delta` is the document's change in
    // length, used to keep a scoped view's end offset in step.
    void applyEdit(uint64_t offset, int64_t delta);

    // Previous / next byte offset respecting UTF-8 sequence boundaries.
    uint64_t prevCharOffset(uint64_t offset) const;
    uint64_t nextCharOffset(uint64_t offset) const;

    // Length of the *addressable* document: the scope end when scoped.
    uint64_t documentLength() const;
    // Real document length, ignoring any scope.
    uint64_t rawDocumentLength() const;

    PieceTable*             m_table       = nullptr;
    SparseLineIndex*        m_index       = nullptr;
    IncrementalHighlighter* m_highlighter = nullptr;

    uint64_t m_firstVisibleLine = 0;
    uint64_t m_cursorOffset     = 0;
    uint64_t m_selAnchor        = 0;
    bool     m_selValid         = false;
    // Column the cursor "wants" when moving vertically through short lines.
    int      m_stickyColumn     = -1;

    uint64_t m_matchStart  = 0;
    uint64_t m_matchLength = 0;

    // Line count the vertical scroll range was last built from. The index
    // learns as it is queried, so a range computed when the document was merely
    // attached (estimate: 1 line) has to be refreshed once it knows better.
    uint64_t m_scrollLineHint = 0;

    bool     m_rangeActive = false;
    uint64_t m_rangeStart  = 0;
    uint64_t m_rangeEnd    = 0;

    int  m_tabWidth   = 4;
    bool m_wordWrap   = false;
    int  m_colMarker1 = 80;
    int  m_colMarker2 = 120;
    bool m_readOnly   = false;
    bool m_darkTheme  = false;

    QFont m_font;
    int   m_lineHeight  = 0;
    int   m_charWidth   = 0;
    int   m_ascent      = 0;

    bool m_caretVisible = true;
    bool m_selecting    = false;

    mutable std::vector<VisualLine> m_lines;
    mutable bool                    m_linesValid = false;

    QScrollBar* m_vScroll = nullptr;
    QScrollBar* m_hScroll = nullptr;
    QTimer*     m_caretTimer = nullptr;
};
