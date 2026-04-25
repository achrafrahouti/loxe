#include "IncrementalHighlighter.h"

IncrementalHighlighter::IncrementalHighlighter()
{
    // Default light-theme colors indexed by Token::Kind
    m_colors = {
        QColor(0x00, 0x55, 0xAA), // TagName            — blue
        QColor(0x99, 0x44, 0x00), // AttrName           — brown
        QColor(0x00, 0x88, 0x00), // AttrValue          — green
        QColor(0x22, 0x22, 0x22), // Text               — near-black
        QColor(0x80, 0x80, 0x80), // Comment            — grey
        QColor(0x00, 0x77, 0x77), // CdataSection       — teal
        QColor(0x88, 0x00, 0x88), // ProcessingInstruction — purple
        QColor(0x66, 0x44, 0x00), // Doctype            — dark amber
        QColor(0xCC, 0x00, 0x00), // EntityRef          — red
        QColor(0x44, 0x44, 0x44), // Punctuation        — dark grey
    };
}

std::vector<std::vector<Token>> IncrementalHighlighter::tokenise(
    const std::vector<QString>& lines,
    const std::vector<QString>& contextLines)
{
    XmlState state = (contextLines == m_cachedContextLines)
        ? m_cachedStateTop
        : computeStateAt(contextLines);

    m_cachedContextLines = contextLines;
    m_cachedStateTop     = state;

    std::vector<std::vector<Token>> result;
    result.reserve(lines.size());
    for (const auto& line : lines)
        result.push_back(tokeniseLine(line, state));
    return result;
}

QColor IncrementalHighlighter::colorFor(Token::Kind kind) const
{
    const auto idx = static_cast<size_t>(kind);
    return idx < m_colors.size() ? m_colors[idx] : QColor(Qt::black);
}

void IncrementalHighlighter::setTheme(const std::vector<QColor>& colors)
{
    m_colors = colors;
    m_cachedContextLines.clear(); // invalidate cache
}

IncrementalHighlighter::XmlState
IncrementalHighlighter::computeStateAt(const std::vector<QString>& contextLines) const
{
    XmlState state = XmlState::Content;
    for (const auto& line : contextLines)
        tokeniseLine(line, state); // side-effect: advances state
    return state;
}

std::vector<Token> IncrementalHighlighter::tokeniseLine(
    const QString& line, XmlState& state) const
{
    std::vector<Token> tokens;
    // TODO: implement XML token state machine per ARC-HL-02
    // Token classes: tag name, attribute name, attribute value, text content,
    // comment, CDATA section, processing instruction, DOCTYPE, entity reference.
    (void)line; (void)state;
    return tokens;
}
