#include "XmlContext.h"
#include "../engine/PieceTable.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

constexpr int kMaxDepth = 128;

struct Tag {
    enum class Kind { Start, End, SelfClosing, Other };
    Kind        kind = Kind::Other;
    QString     name;
    uint64_t    offset = 0; // offset of the '<'
    std::string raw;
};

bool isNameChar(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.' || u >= 0x80;
}

// Classifies the markup starting at `raw[0] == '<'`.
Tag classify(std::string raw, uint64_t offset)
{
    Tag t;
    t.offset = offset;
    t.raw    = std::move(raw);
    const std::string& s = t.raw;

    if (s.size() < 2) return t;
    if (s[1] == '?' || s[1] == '!') return t; // PI, comment, CDATA, DOCTYPE

    size_t i = 1;
    if (s[i] == '/') { t.kind = Tag::Kind::End; ++i; }
    else             { t.kind = Tag::Kind::Start; }

    const size_t nameStart = i;
    while (i < s.size() && isNameChar(s[i])) ++i;
    if (i == nameStart) { t.kind = Tag::Kind::Other; return t; }
    t.name = QString::fromUtf8(s.data() + nameStart, static_cast<int>(i - nameStart));

    if (t.kind == Tag::Kind::Start && s.size() >= 2 && s[s.size() - 2] == '/')
        t.kind = Tag::Kind::SelfClosing;
    return t;
}

// Parses the attributes of a start tag's raw text.
QList<QPair<QString, QString>> parseAttributes(const std::string& raw)
{
    QList<QPair<QString, QString>> out;
    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i; // skip the element name

    while (i < raw.size()) {
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') break;

        const size_t nameStart = i;
        while (i < raw.size() && isNameChar(raw[i])) ++i;
        if (i == nameStart) { ++i; continue; }
        const QString name = QString::fromUtf8(raw.data() + nameStart,
                                               static_cast<int>(i - nameStart));

        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size() || raw[i] != '=') { out.append({name, {}}); continue; }
        ++i;
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size()) break;

        QString value;
        if (raw[i] == '"' || raw[i] == '\'') {
            const char quote = raw[i++];
            const size_t valStart = i;
            while (i < raw.size() && raw[i] != quote) ++i;
            value = QString::fromUtf8(raw.data() + valStart, static_cast<int>(i - valStart));
            if (i < raw.size()) ++i;
        } else {
            const size_t valStart = i;
            while (i < raw.size() && !std::isspace(static_cast<unsigned char>(raw[i]))
                   && raw[i] != '>' && raw[i] != '/') ++i;
            value = QString::fromUtf8(raw.data() + valStart, static_cast<int>(i - valStart));
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
    if (offset == 0) return info;

    const uint64_t floorOffset = (offset > budget) ? offset - budget : 0;
    if (floorOffset > 0) info.truncated = true;

    // Read the whole window at once: the budget caps it at a few MB.
    const uint64_t windowLen = offset - floorOffset;
    const std::string window  = doc.read(floorOffset, windowLen);
    if (window.empty()) return info;

    // Walk backwards over '<' positions, matching end tags to start tags.
    std::vector<Tag> ancestors;
    int  pendingClose = 0;
    auto pos          = static_cast<long long>(window.size()) - 1;

    while (pos >= 0 && static_cast<int>(ancestors.size()) < kMaxDepth) {
        // Find the previous '<'.
        while (pos >= 0 && window[static_cast<size_t>(pos)] != '<') --pos;
        if (pos < 0) break;

        // Take the tag's text up to its '>' (may run past the cursor, which is
        // exactly what we want when the cursor sits inside a start tag).
        const size_t start = static_cast<size_t>(pos);
        size_t       end   = window.find('>', start);
        std::string  raw;
        if (end == std::string::npos) {
            // Unterminated within the window — read forward from the document.
            const std::string ahead = doc.read(floorOffset + start, 8192);
            const size_t gt = ahead.find('>');
            raw = (gt == std::string::npos) ? ahead : ahead.substr(0, gt + 1);
        } else {
            raw = window.substr(start, end - start + 1);
        }

        const Tag tag = classify(std::move(raw), floorOffset + start);
        switch (tag.kind) {
        case Tag::Kind::End:
            ++pendingClose;
            break;
        case Tag::Kind::Start:
            if (pendingClose > 0) --pendingClose;
            else                  ancestors.push_back(tag);
            break;
        case Tag::Kind::SelfClosing:
        case Tag::Kind::Other:
            break;
        }

        --pos;
    }

    // ancestors was built innermost-first.
    std::reverse(ancestors.begin(), ancestors.end());
    for (const Tag& t : ancestors) info.ancestors << t.name;

    if (!ancestors.empty()) {
        const Tag& innermost = ancestors.back();
        info.tagName    = innermost.name;
        info.attributes = parseAttributes(innermost.raw);
    }
    return info;
}

} // namespace XmlContext
