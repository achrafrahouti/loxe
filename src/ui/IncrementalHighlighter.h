#pragma once

#include <QColor>
#include <QString>
#include <vector>

// A single coloured token within a line.
struct Token {
    enum class Kind {
        TagName,
        AttrName,
        AttrValue,
        Text,
        Comment,
        CdataSection,
        ProcessingInstruction,
        Doctype,
        EntityRef,
        Punctuation,
    };
    Kind kind;
    int  start;  // byte offset within the line string
    int  length;
};

// Tokenises only the visible viewport lines (plus a context window above)
// synchronously from paintEvent. Caches parser state at the top of the
// viewport so identical repaints do not re-scan context lines.
class IncrementalHighlighter {
public:
    IncrementalHighlighter();

    // contextLines: lines above the viewport used to establish parser state.
    // Returns one token list per element of lines, in the same order.
    std::vector<std::vector<Token>> tokenise(
        const std::vector<QString>& lines,
        const std::vector<QString>& contextLines);

    QColor colorFor(Token::Kind kind) const;

    // Replace the active color theme.
    void setTheme(const std::vector<QColor>& colors);

private:
    enum class XmlState {
        Content,
        TagOpen,
        TagName,
        EndTagName,
        AttrName,
        AttrEquals,
        AttrValueSingle,
        AttrValueDouble,
        Comment,
        Cdata,
        Pi,
        Doctype,
    };

    XmlState computeStateAt(const std::vector<QString>& contextLines) const;
    std::vector<Token> tokeniseLine(const QString& line, XmlState& state) const;

    XmlState             m_cachedStateTop{XmlState::Content};
    std::vector<QString> m_cachedContextLines;
    std::vector<QColor>  m_colors; // indexed by Token::Kind
};
