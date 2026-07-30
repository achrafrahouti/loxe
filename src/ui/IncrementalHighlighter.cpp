#include "IncrementalHighlighter.h"

namespace {

bool isNameStart(QChar c)
{
    return c.isLetter() || c == '_' || c == ':';
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == '_' || c == ':' || c == '-' || c == '.';
}

} // namespace

IncrementalHighlighter::IncrementalHighlighter()
{
    m_colors = lightTheme();
}

std::vector<QColor> IncrementalHighlighter::lightTheme()
{
    return {
        QColor(0x00, 0x55, 0xAA), // TagName               — blue
        QColor(0x99, 0x44, 0x00), // AttrName              — brown
        QColor(0x00, 0x88, 0x00), // AttrValue             — green
        QColor(0x22, 0x22, 0x22), // Text                  — near-black
        QColor(0x80, 0x80, 0x80), // Comment               — grey
        QColor(0x00, 0x77, 0x77), // CdataSection          — teal
        QColor(0x88, 0x00, 0x88), // ProcessingInstruction — purple
        QColor(0x66, 0x44, 0x00), // Doctype               — dark amber
        QColor(0xCC, 0x00, 0x00), // EntityRef             — red
        QColor(0x44, 0x44, 0x44), // Punctuation           — dark grey
    };
}

std::vector<QColor> IncrementalHighlighter::darkTheme()
{
    return {
        QColor(0x6C, 0xB6, 0xFF), // TagName
        QColor(0xE8, 0xA8, 0x6A), // AttrName
        QColor(0x8D, 0xD9, 0x7F), // AttrValue
        QColor(0xD8, 0xD8, 0xD8), // Text
        QColor(0x7F, 0x8C, 0x98), // Comment
        QColor(0x5F, 0xD0, 0xD0), // CdataSection
        QColor(0xD8, 0x9A, 0xE6), // ProcessingInstruction
        QColor(0xD4, 0xB3, 0x6A), // Doctype
        QColor(0xFF, 0x7B, 0x72), // EntityRef
        QColor(0xA0, 0xA8, 0xB0), // Punctuation
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

// One pass over the line, appending a token per contiguous run of one kind.
// `state` carries over between lines so comments, CDATA sections, PIs, DOCTYPEs
// and multi-line attribute values keep their colour across line breaks.
std::vector<Token> IncrementalHighlighter::tokeniseLine(
    const QString& line, XmlState& state) const
{
    std::vector<Token> tokens;
    const int n = line.size();
    int       i = 0;

    auto push = [&tokens](Token::Kind kind, int start, int length) {
        if (length <= 0) return;
        // Merge with the previous token when it is the same kind and adjacent,
        // so a run split across loop iterations paints as one drawText call.
        if (!tokens.empty()) {
            Token& back = tokens.back();
            if (back.kind == kind && back.start + back.length == start) {
                back.length += length;
                return;
            }
        }
        tokens.push_back({kind, start, length});
    };

    while (i < n) {
        switch (state) {
        case XmlState::Comment: {
            const int end = line.indexOf("-->", i);
            if (end < 0) { push(Token::Kind::Comment, i, n - i); i = n; }
            else { push(Token::Kind::Comment, i, end + 3 - i); i = end + 3; state = XmlState::Content; }
            break;
        }

        case XmlState::Cdata: {
            const int end = line.indexOf("]]>", i);
            if (end < 0) { push(Token::Kind::CdataSection, i, n - i); i = n; }
            else { push(Token::Kind::CdataSection, i, end + 3 - i); i = end + 3; state = XmlState::Content; }
            break;
        }

        case XmlState::Pi: {
            const int end = line.indexOf("?>", i);
            if (end < 0) { push(Token::Kind::ProcessingInstruction, i, n - i); i = n; }
            else { push(Token::Kind::ProcessingInstruction, i, end + 2 - i); i = end + 2; state = XmlState::Content; }
            break;
        }

        case XmlState::Doctype: {
            const int end = line.indexOf('>', i);
            if (end < 0) { push(Token::Kind::Doctype, i, n - i); i = n; }
            else { push(Token::Kind::Doctype, i, end + 1 - i); i = end + 1; state = XmlState::Content; }
            break;
        }

        case XmlState::AttrValueSingle:
        case XmlState::AttrValueDouble: {
            const QChar quote = (state == XmlState::AttrValueSingle) ? QLatin1Char('\'') : QLatin1Char('"');
            const int   end   = line.indexOf(quote, i);
            if (end < 0) { push(Token::Kind::AttrValue, i, n - i); i = n; }
            else { push(Token::Kind::AttrValue, i, end + 1 - i); i = end + 1; state = XmlState::AttrName; }
            break;
        }

        case XmlState::Content: {
            if (line[i] == '<') {
                // Decide which flavour of markup begins here.
                if (line.mid(i, 4) == QLatin1String("<!--")) {
                    state = XmlState::Comment;
                } else if (line.mid(i, 9) == QLatin1String("<![CDATA[")) {
                    push(Token::Kind::CdataSection, i, 9);
                    i += 9;
                    state = XmlState::Cdata;
                } else if (line.mid(i, 2) == QLatin1String("<?")) {
                    state = XmlState::Pi;
                } else if (line.mid(i, 2) == QLatin1String("<!")) {
                    state = XmlState::Doctype;
                } else {
                    push(Token::Kind::Punctuation, i, 1);
                    ++i;
                    if (i < n && line[i] == '/') { push(Token::Kind::Punctuation, i, 1); ++i; }
                    state = XmlState::TagName;
                }
                break;
            }

            if (line[i] == '&') {
                // Entity reference: &name; or &#123; — only if terminated.
                const int semi = line.indexOf(';', i);
                if (semi > i && semi - i <= 12) {
                    push(Token::Kind::EntityRef, i, semi + 1 - i);
                    i = semi + 1;
                } else {
                    push(Token::Kind::Text, i, 1);
                    ++i;
                }
                break;
            }

            // Plain text up to the next markup or entity.
            int j = i;
            while (j < n && line[j] != '<' && line[j] != '&') ++j;
            push(Token::Kind::Text, i, j - i);
            i = j;
            break;
        }

        case XmlState::TagName: {
            if (isNameStart(line[i])) {
                int j = i + 1;
                while (j < n && isNameChar(line[j])) ++j;
                push(Token::Kind::TagName, i, j - i);
                i = j;
            } else if (line[i] != '>' && line[i] != '/' && !line[i].isSpace()) {
                // Neither a name nor tag punctuation — consume one byte as
                // punctuation so the loop always advances.
                push(Token::Kind::Punctuation, i, 1);
                ++i;
            }
            // '>', '/' and whitespace are left for the AttrName state.
            state = XmlState::AttrName;
            break;
        }

        case XmlState::AttrName: {
            if (line[i].isSpace()) {
                push(Token::Kind::Text, i, 1);
                ++i;
                break;
            }
            if (line[i] == '>') {
                push(Token::Kind::Punctuation, i, 1);
                ++i;
                state = XmlState::Content;
                break;
            }
            if (line[i] == '/') {
                push(Token::Kind::Punctuation, i, 1);
                ++i;
                break;
            }
            if (line[i] == '=') {
                push(Token::Kind::Punctuation, i, 1);
                ++i;
                state = XmlState::AttrEquals;
                break;
            }
            int j = i;
            while (j < n && isNameChar(line[j])) ++j;
            if (j == i) { push(Token::Kind::Punctuation, i, 1); ++i; break; }
            push(Token::Kind::AttrName, i, j - i);
            i = j;
            break;
        }

        case XmlState::AttrEquals: {
            if (line[i].isSpace()) { push(Token::Kind::Text, i, 1); ++i; break; }
            if (line[i] == '"')  { push(Token::Kind::AttrValue, i, 1); ++i; state = XmlState::AttrValueDouble; break; }
            if (line[i] == '\'') { push(Token::Kind::AttrValue, i, 1); ++i; state = XmlState::AttrValueSingle; break; }
            // Unquoted value (not well-formed XML, but colour it anyway).
            int j = i;
            while (j < n && !line[j].isSpace() && line[j] != '>' && line[j] != '/') ++j;
            push(Token::Kind::AttrValue, i, j - i);
            i     = (j > i) ? j : i + 1;
            state = XmlState::AttrName;
            break;
        }

        // States the scanner never rests in mid-line.
        case XmlState::TagOpen:
        case XmlState::EndTagName:
            state = XmlState::TagName;
            break;
        }
    }

    return tokens;
}
