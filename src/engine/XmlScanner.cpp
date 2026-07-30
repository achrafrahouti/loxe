#include "XmlScanner.h"
#include "PieceTable.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace {

bool isNameChar(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.' || u >= 0x80;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool endsWith(const std::string& s, std::string_view suffix)
{
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Reads the element name out of a tag's raw text, e.g. "</foo >" → "foo".
std::string_view tagName(std::string_view raw)
{
    size_t i = 1;
    if (i < raw.size() && (raw[i] == '/')) ++i;
    const size_t start = i;
    while (i < raw.size() && isNameChar(raw[i])) ++i;
    return raw.substr(start, i - start);
}

// Classifies a complete tag from its raw text.
XmlNode::Kind classifyTag(std::string_view raw)
{
    if (raw.size() >= 2 && raw[1] == '/')                 return XmlNode::Kind::EndTag;
    if (raw.size() >= 2 && raw[raw.size() - 2] == '/')    return XmlNode::Kind::EmptyTag;
    return XmlNode::Kind::StartTag;
}

// The scanner's position within a node. Mirrors the grammar closely enough that
// each state maps to exactly one terminator to search for.
enum class S {
    Text, TagStart, Bang, Comment, Cdata, Doctype, Pi, Tag, TagQuoteS, TagQuoteD
};

} // namespace

bool XmlScanner::scan(const PieceTable& doc, uint64_t from, uint64_t to,
                      const NodeFn& cb, const std::atomic<bool>* cancelled)
{
    if (!cb) return false;
    const uint64_t docLen = doc.length();
    to   = std::min(to, docLen);
    from = std::min(from, to);

    S           state = S::Text;
    std::string node;          // the node currently being accumulated
    uint64_t    nodeStart = from;
    int         bracketDepth = 0;

    XmlNode out;

    // Emits one node. `raw` may alias the read buffer (fast path, no copy) or
    // the `node` accumulator (slow path, for nodes spanning a chunk boundary).
    auto emit = [&](XmlNode::Kind kind, std::string_view raw, uint64_t offset) {
        out.kind   = kind;
        out.offset = offset;
        out.raw    = raw;
        out.name   = (kind == XmlNode::Kind::StartTag || kind == XmlNode::Kind::EndTag
                      || kind == XmlNode::Kind::EmptyTag)
            ? tagName(raw) : std::string_view();
        return cb(out);
    };

    auto deliver = [&](XmlNode::Kind kind) { return emit(kind, node, nodeStart); };

    std::string buf;
    buf.resize(64 * 1024);

    // Release file pages behind us as we go. Scanning a 2 GB document
    // otherwise leaves the whole mapping resident and blows the memory budget.
    constexpr uint64_t kReleaseEvery = 8ull * 1024 * 1024;
    uint64_t lastRelease = from;

    uint64_t pos = from;
    while (pos < to) {
        if (cancelled && cancelled->load()) return false;

        if (pos - lastRelease >= kReleaseEvery) {
            doc.releasePagesBefore(pos);
            lastRelease = pos;
        }

        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), to - pos));
        const size_t got  = doc.readInto(pos, buf.data(), want);
        if (got == 0) break;

        for (size_t k = 0; k < got; ) {
            // ---- Fast path -------------------------------------------------
            // At a clean node boundary, consume whole nodes that fit inside this
            // chunk and hand them out as views into the read buffer. This avoids
            // copying every node through the `node` accumulator, which dominates
            // the cost on element-dense documents. Anything that crosses the
            // chunk boundary, or needs stateful handling, falls through to the
            // per-byte machine below.
            if (state == S::Text && node.empty()) {
                const char* const b = buf.data();
                bool spanning = false;

                while (k < got) {
                    if (b[k] != '<') {
                        const auto* lt = static_cast<const char*>(
                            ::memchr(b + k, '<', got - k));
                        if (!lt) { spanning = true; break; } // text continues past chunk
                        const size_t len = static_cast<size_t>(lt - (b + k));
                        if (len > kMaxTextNode) { spanning = true; break; }
                        if (!emit(XmlNode::Kind::Text, {b + k, len}, pos + k)) return false;
                        k += len;
                        continue;
                    }

                    // Markup. Only plain tags are handled here; comments, CDATA,
                    // PIs and DOCTYPEs are rare and go to the slow path.
                    if (k + 1 >= got) { spanning = true; break; }
                    if (b[k + 1] == '?' || b[k + 1] == '!') { spanning = true; break; }

                    size_t j = k + 1;
                    while (j < got) {
                        const char ch = b[j];
                        if (ch == '>') break;
                        if (ch == '"' || ch == '\'') {
                            // Skip a quoted attribute value in one step.
                            const auto* q = static_cast<const char*>(
                                ::memchr(b + j + 1, ch, got - j - 1));
                            if (!q) { j = got; break; }
                            j = static_cast<size_t>(q - b) + 1;
                            continue;
                        }
                        ++j;
                    }
                    if (j >= got) { spanning = true; break; } // tag crosses chunk

                    const std::string_view raw{b + k, j - k + 1};
                    if (!emit(classifyTag(raw), raw, pos + k)) return false;
                    k = j + 1;
                }

                if (!spanning) break;  // chunk fully consumed on the fast path
                // else: fall through with `state == S::Text` and k at the
                // start of the node that straddles the boundary.
            }

            // ---- Slow path -------------------------------------------------
            // Text is the bulk of most documents, so jump to the next '<' with
            // memchr instead of stepping a byte at a time.
            if (state == S::Text) {
                const auto* lt = static_cast<const char*>(
                    ::memchr(buf.data() + k, '<', got - k));
                const size_t stop = lt ? static_cast<size_t>(lt - buf.data()) : got;

                // Take the run in kMaxTextNode-sized bites so an oversized text
                // node is split rather than overshooting the cap by a chunk.
                while (k < stop) {
                    if (node.empty()) nodeStart = pos + k;
                    const size_t take = std::min(stop - k, kMaxTextNode - node.size());
                    node.append(buf.data() + k, take);
                    k += take;
                    if (node.size() >= kMaxTextNode) {
                        if (!deliver(XmlNode::Kind::Text)) return false;
                        node.clear();
                    }
                }
                if (k >= got) break;

                // buf[k] is '<': close any pending text node and open markup.
                if (!node.empty() && !deliver(XmlNode::Kind::Text)) return false;
                node.assign(1, '<');
                nodeStart = pos + k;
                state     = S::TagStart;
                ++k;
                continue;
            }

            const char c = buf[k];
            ++k;

            switch (state) {
            case S::Text:
                break; // handled above

            case S::TagStart:
                node.push_back(c);
                if (c == '?')      state = S::Pi;
                else if (c == '!') state = S::Bang;
                else               state = S::Tag; // start tag or end tag
                break;

            case S::Bang:
                node.push_back(c);
                if (node == "<!--")           state = S::Comment;
                else if (node == "<![CDATA[") state = S::Cdata;
                else if (node.size() >= 3 && node.compare(0, 3, "<!-") != 0
                                          && node.compare(0, 3, "<![") != 0) {
                    bracketDepth = 0;
                    state = S::Doctype;
                }
                break;

            case S::Comment:
                node.push_back(c);
                if (endsWith(node, "-->")) {
                    if (!deliver(XmlNode::Kind::Comment)) return false;
                    node.clear();
                    state = S::Text;
                }
                break;

            case S::Cdata:
                node.push_back(c);
                if (endsWith(node, "]]>")) {
                    if (!deliver(XmlNode::Kind::Cdata)) return false;
                    node.clear();
                    state = S::Text;
                }
                break;

            case S::Pi:
                node.push_back(c);
                if (endsWith(node, "?>")) {
                    if (!deliver(XmlNode::Kind::ProcessingInstruction)) return false;
                    node.clear();
                    state = S::Text;
                }
                break;

            case S::Doctype:
                node.push_back(c);
                // '>' inside an internal subset does not end the DOCTYPE.
                if (c == '[') ++bracketDepth;
                else if (c == ']') { if (bracketDepth > 0) --bracketDepth; }
                else if (c == '>' && bracketDepth == 0) {
                    if (!deliver(XmlNode::Kind::Doctype)) return false;
                    node.clear();
                    state = S::Text;
                }
                break;

            case S::Tag:
                node.push_back(c);
                if (c == '\'')     state = S::TagQuoteS;
                else if (c == '"') state = S::TagQuoteD;
                else if (c == '>') {
                    if (!deliver(classifyTag(node))) return false;
                    node.clear();
                    state = S::Text;
                }
                break;

            case S::TagQuoteS:
                node.push_back(c);
                if (c == '\'') state = S::Tag;
                break;

            case S::TagQuoteD:
                node.push_back(c);
                if (c == '"') state = S::Tag;
                break;
            }
        }

        pos += got;
    }

    // Whatever is left is an unterminated node; report text as-is and drop
    // partial markup, which only happens on malformed input.
    if (!node.empty() && state == S::Text) {
        if (!deliver(XmlNode::Kind::Text)) return false;
    }
    return true;
}

bool XmlScanner::scanAll(const PieceTable& doc, const NodeFn& cb,
                         const std::atomic<bool>* cancelled)
{
    return scan(doc, 0, doc.length(), cb, cancelled);
}

bool XmlScanner::firstAttribute(std::string_view raw,
                                std::string_view* name, std::string_view* value)
{
    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i; // element name

    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') return false;

    const size_t nameStart = i;
    while (i < raw.size() && isNameChar(raw[i])) ++i;
    if (i == nameStart) return false;
    *name = raw.substr(nameStart, i - nameStart);

    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size() || raw[i] != '=') { *value = {}; return true; }
    ++i;
    while (i < raw.size() && isSpace(raw[i])) ++i;
    if (i >= raw.size()) { *value = {}; return true; }

    if (raw[i] == '"' || raw[i] == '\'') {
        const char quote = raw[i++];
        const size_t start = i;
        while (i < raw.size() && raw[i] != quote) ++i;
        *value = raw.substr(start, i - start);
    } else {
        const size_t start = i;
        while (i < raw.size() && !isSpace(raw[i]) && raw[i] != '>' && raw[i] != '/') ++i;
        *value = raw.substr(start, i - start);
    }
    return true;
}

std::string_view XmlScanner::attributeValue(std::string_view raw, std::string_view attr)
{
    size_t i = 1;
    if (i < raw.size() && raw[i] == '/') ++i;
    while (i < raw.size() && isNameChar(raw[i])) ++i;

    while (i < raw.size()) {
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] == '>' || raw[i] == '/') break;

        const size_t nameStart = i;
        while (i < raw.size() && isNameChar(raw[i])) ++i;
        if (i == nameStart) { ++i; continue; }
        const std::string_view name = raw.substr(nameStart, i - nameStart);

        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size() || raw[i] != '=') continue;
        ++i;
        while (i < raw.size() && isSpace(raw[i])) ++i;
        if (i >= raw.size()) break;

        std::string_view value;
        if (raw[i] == '"' || raw[i] == '\'') {
            const char quote = raw[i++];
            const size_t start = i;
            while (i < raw.size() && raw[i] != quote) ++i;
            value = raw.substr(start, i - start);
            if (i < raw.size()) ++i;
        } else {
            const size_t start = i;
            while (i < raw.size() && !isSpace(raw[i]) && raw[i] != '>' && raw[i] != '/') ++i;
            value = raw.substr(start, i - start);
        }
        if (name == attr) return value;
    }
    return {};
}
