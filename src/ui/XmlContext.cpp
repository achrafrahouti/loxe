#include "XmlContext.h"
#include "../engine/PieceTable.h"
#include "../engine/XmlScanner.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace {

constexpr int kMaxDepth = 128;

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isNameChar(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.' || u >= 0x80;
}

// Parses the attributes out of a start tag's raw source, e.g.
// `<item id="5" name='z'>` → {{id,5},{name,z}}.
QList<QPair<QString, QString>> parseAttributes(std::string_view raw)
{
    QList<QPair<QString, QString>> out;

    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i; // skip the element name

    while (i < raw.size() && out.size() < 256) {
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') break;

        const size_t nameStart = i;
        while (i < raw.size() && isNameChar(raw[i])) ++i;
        if (i == nameStart) { ++i; continue; } // not a name — skip the byte
        const QString name = qs(raw.substr(nameStart, i - nameStart));

        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] != '=') { out.append({name, {}}); continue; }
        ++i;
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size()) break;

        QString value;
        if (raw[i] == '"' || raw[i] == '\'') {
            const char quote = raw[i++];
            const size_t valStart = i;
            while (i < raw.size() && raw[i] != quote) ++i;
            value = qs(raw.substr(valStart, i - valStart));
            if (i < raw.size()) ++i;
        } else {
            const size_t valStart = i;
            while (i < raw.size() && !isSpace(raw[i]) && raw[i] != '>' && raw[i] != '/') ++i;
            value = qs(raw.substr(valStart, i - valStart));
        }
        out.append({name, value});
    }
    return out;
}

} // namespace

namespace XmlContext {

XmlContextInfo contextAt(const PieceTable& doc, uint64_t offset, uint64_t budget)
{
    XmlContextInfo info;
    const uint64_t docLen = doc.length();
    offset = std::min(offset, docLen);

    // --- Ancestors -----------------------------------------------------------
    //
    // Scan *forward* from the cursor. An element encloses the cursor exactly
    // when its end tag turns up with no matching start tag in between, so the
    // unmatched end tags are the ancestor chain, innermost first.
    //
    // Going forwards rather than backwards is what makes this correct: the
    // scanner understands comments, CDATA, processing instructions and quoted
    // attribute values, so a '<' inside any of them cannot be mistaken for a
    // tag. It also means an unclosed element before the cursor (an HTML-style
    // <br>, say) is simply never seen, instead of being reported as an ancestor.
    const uint64_t forwardEnd = std::min(docLen, offset + budget);

    std::vector<QString> inner;   // innermost → outermost
    int depth = 0;

    XmlScanner::scan(doc, offset, forwardEnd, [&](const XmlNode& n) {
        switch (n.kind) {
        case XmlNode::Kind::StartTag:
            ++depth;
            break;
        case XmlNode::Kind::EndTag:
            if (depth == 0) {
                inner.push_back(qs(n.name));
                if (static_cast<int>(inner.size()) >= kMaxDepth) return false;
            } else {
                --depth;
            }
            break;
        default:
            break;
        }
        return true;
    });

    // Running out of budget before the outermost element closed means the
    // chain is missing its top; say so rather than showing a wrong root.
    if (forwardEnd < docLen && static_cast<int>(inner.size()) < kMaxDepth)
        info.truncated = true;

    for (auto it = inner.rbegin(); it != inner.rend(); ++it)
        info.ancestors << *it;

    if (info.ancestors.isEmpty()) return info;
    info.tagName = info.ancestors.last();

    // --- Attributes of the innermost element ---------------------------------
    //
    // Its start tag is behind the cursor. Scan a bounded window up to the cursor
    // and keep the last start tag carrying that name: the innermost open element
    // is by definition the most recent unclosed one.
    const uint64_t windowStart = (offset > budget) ? offset - budget : 0;
    const std::string wanted   = info.tagName.toStdString();

    // The window runs past the cursor so that a start tag the cursor sits
    // *inside* is still seen whole; `offset` only has to fall within the tag,
    // which the offset test below enforces.
    std::string lastRaw;
    XmlScanner::scan(doc, windowStart, forwardEnd, [&](const XmlNode& n) {
        if (n.offset > offset) return false; // past the cursor: done
        if (n.kind == XmlNode::Kind::StartTag && n.name == wanted)
            lastRaw.assign(n.raw);
        return true;
    });

    if (!lastRaw.empty())
        info.attributes = parseAttributes(lastRaw);

    return info;
}

} // namespace XmlContext
