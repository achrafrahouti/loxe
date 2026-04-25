#pragma once

#include <QScrollBar>
#include <QWidget>
#include <cstdint>

class PieceTable;
class SparseLineIndex;
class IncrementalHighlighter;

// Custom QWidget that paints only the visible lines of the document (~40–80).
// Cursor position is stored as a byte offset, not (line, col), so it survives edits.
// Emits cursorMoved() on every cursor movement for the tree view and breadcrumb.
class ViewportRenderer : public QWidget {
    Q_OBJECT
public:
    explicit ViewportRenderer(QWidget* parent = nullptr);
    ~ViewportRenderer() override;

    void setDocument(PieceTable* table, SparseLineIndex* index);

    uint64_t cursorOffset()     const { return m_cursorOffset; }
    int      tabWidth()         const { return m_tabWidth; }
    bool     wordWrap()         const { return m_wordWrap; }

    void setCursorOffset(uint64_t offset);
    void setTabWidth(int spaces);
    void setWordWrap(bool on);
    void setColumnMarkers(int col1, int col2); // 0 to disable

signals:
    void cursorMoved(uint64_t byteOffset);

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    // Number of lines that fit in the widget (ceil(height/lineHeight) + 2).
    int visibleLineCount() const;

    // Fetch up to visibleLineCount()+2 lines from PieceTable via SparseLineIndex.
    std::vector<QString> fetchVisibleLines() const;

    void scrollToLine(uint64_t line);
    void updateScrollBars();

    // Pixel x-coordinate of the gutter right edge.
    int gutterWidth() const;

    PieceTable*             m_table       = nullptr;
    SparseLineIndex*        m_index       = nullptr;
    IncrementalHighlighter* m_highlighter = nullptr;

    uint64_t m_firstVisibleLine = 0;
    uint64_t m_cursorOffset     = 0;
    int      m_cursorVisualCol  = 0;
    int      m_tabWidth         = 4;
    bool     m_wordWrap         = false;
    int      m_colMarker1       = 80;
    int      m_colMarker2       = 120;
    int      m_lineHeight       = 0; // computed on first paint

    QScrollBar* m_vScroll = nullptr;
    QScrollBar* m_hScroll = nullptr;
};
