#pragma once

#include "PieceTable.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// A single markup or text node reported by XmlScanner.
struct XmlNode {
    enum class Kind {
        Text,
        StartTag,             // <a ...>
        EndTag,               // </a>
        EmptyTag,             // <a .../>
        Comment,              // <!-- ... -->
        Cdata,                // <![CDATA[ ... ]]>
        ProcessingInstruction, // <? ... ?>
        Doctype,              // <!DOCTYPE ... >
    };

    Kind             kind   = Kind::Text;
    uint64_t         offset = 0;  // byte offset of the node's first byte
    std::string_view raw;         // the node's full text; valid for this call only
    std::string_view name;        // element name, for Start/End/Empty tags only
};

// Streaming XML tokeniser over a PieceTable.
//
// The whole point is that it never holds more than one node in memory, so it
// works on a 2 GB document within the resident-memory budget. It is deliberately
// a scanner rather than a validating parser: no DTD handling, no namespace
// resolution, no well-formedness enforcement. Validator (libxml2) covers that;
// this exists because the tree needs byte offsets and a node-at-a-time cursor,
// which a validating parser does not give us.
//
// This is the shared tokeniser behind both FormatEngine and VirtualTreeModel.
//
// scan() is a template on the callback rather than taking a std::function: a
// dense document produces tens of millions of nodes, and one non-inlinable
// indirect call each is a measurable share of the total scan time.
class XmlScanner {
public:
    // Callbacks return false to stop the scan early. Kept as a named type for
    // callers that want to store one; scan() accepts any compatible callable.
    using NodeFn = std::function<bool(const XmlNode&)>;

    // Text nodes longer than this are split across consecutive Text callbacks
    // so a document that is one huge text node still streams in bounded memory.
    static constexpr size_t kMaxTextNode = 256 * 1024;

    // Bytes copied out of the PieceTable per read.
    static constexpr size_t kReadChunk = 64 * 1024;

    // Scans [from, to) of `doc`. Returns false if the callback stopped the scan
    // or the cancellation flag was raised.
    template <class F>
    static bool scan(const PieceTable& doc, uint64_t from, uint64_t to,
                     F&& cb, const std::atomic<bool>* cancelled = nullptr);

    template <class F>
    static bool scanAll(const PieceTable& doc, F&& cb,
                        const std::atomic<bool>* cancelled = nullptr)
    {
        return scan(doc, 0, doc.length(), std::forward<F>(cb), cancelled);
    }

    // Extracts the value of `attr` from a start-tag's raw text, or an empty
    // view if absent. Returned view points into `rawTag`.
    static std::string_view attributeValue(std::string_view rawTag, std::string_view attr);

    // Extracts the first attribute of a start tag. Returns false if it has none.
    static bool firstAttribute(std::string_view rawTag,
                               std::string_view* name, std::string_view* value);

    // Implementation details shared with XmlScanner.cpp.
    struct Detail {
        // Name characters are classified from a table rather than std::isalnum,
        // which is locale-dependent and reaches through __ctype_b_loc() — a
        // thread-local lookup per character. tagName() runs over every tag in
        // the document, so that call showed up as real time on a dense file.
        struct NameCharTable {
            bool v[256] = {};
            constexpr NameCharTable()
            {
                for (int c = '0'; c <= '9'; ++c) v[c] = true;
                for (int c = 'A'; c <= 'Z'; ++c) v[c] = true;
                for (int c = 'a'; c <= 'z'; ++c) v[c] = true;
                v[static_cast<unsigned char>('_')] = true;
                v[static_cast<unsigned char>(':')] = true;
                v[static_cast<unsigned char>('-')] = true;
                v[static_cast<unsigned char>('.')] = true;
                for (int c = 0x80; c < 256; ++c) v[c] = true; // UTF-8 lead/continuation
            }
        };

        static bool isNameChar(char c)
        {
            static constexpr NameCharTable table{};
            return table.v[static_cast<unsigned char>(c)];
        }

        // Reads the element name out of a tag's raw text, e.g. "</foo >" → "foo".
        static std::string_view tagName(std::string_view raw)
        {
            size_t i = 1;
            if (i < raw.size() && raw[i] == '/') ++i;
            const size_t start = i;
            while (i < raw.size() && isNameChar(raw[i])) ++i;
            return raw.substr(start, i - start);
        }

        // Classifies a complete tag from its raw text.
        static XmlNode::Kind classifyTag(std::string_view raw)
        {
            if (raw.size() >= 2 && raw[1] == '/')              return XmlNode::Kind::EndTag;
            if (raw.size() >= 2 && raw[raw.size() - 2] == '/') return XmlNode::Kind::EmptyTag;
            return XmlNode::Kind::StartTag;
        }

        static bool endsWith(const std::string& s, std::string_view suffix)
        {
            return s.size() >= suffix.size()
                && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    };
};

// The scanner's position within a node. Mirrors the grammar closely enough that
// each state maps to exactly one terminator to search for.
namespace xmlscanner_detail {
enum class S {
    Text, TagStart, Bang, Comment, Cdata, Doctype, Pi, Tag, TagQuoteS, TagQuoteD
};
} // namespace xmlscanner_detail

template <class F>
bool XmlScanner::scan(const PieceTable& doc, uint64_t from, uint64_t to,
                      F&& cb, const std::atomic<bool>* cancelled)
{
    using xmlscanner_detail::S;
    using D = XmlScanner::Detail;

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
    auto emitNode = [&](XmlNode::Kind kind, std::string_view raw, uint64_t offset) {
        out.kind   = kind;
        out.offset = offset;
        out.raw    = raw;
        out.name   = (kind == XmlNode::Kind::StartTag || kind == XmlNode::Kind::EndTag
                      || kind == XmlNode::Kind::EmptyTag)
            ? D::tagName(raw) : std::string_view();
        return static_cast<bool>(cb(out));
    };

    auto deliver = [&](XmlNode::Kind kind) { return emitNode(kind, node, nodeStart); };

    std::string buf;
    buf.resize(kReadChunk);

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
                        if (!emitNode(XmlNode::Kind::Text, {b + k, len}, pos + k)) return false;
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
                    if (!emitNode(D::classifyTag(raw), raw, pos + k)) return false;
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

            // Comments, CDATA sections and PIs all end with '>', so jump to the
            // next one with memchr and test the full terminator against the
            // accumulated node, instead of stepping a byte at a time. Testing
            // `node` rather than the buffer keeps a terminator split across a
            // chunk boundary working. A multi-megabyte comment or CDATA island
            // is otherwise by far the slowest thing in the scanner.
            if (state == S::Comment || state == S::Cdata || state == S::Pi) {
                const std::string_view term =
                    (state == S::Comment) ? std::string_view("-->")
                  : (state == S::Cdata)   ? std::string_view("]]>")
                                          : std::string_view("?>");
                const XmlNode::Kind kind =
                    (state == S::Comment) ? XmlNode::Kind::Comment
                  : (state == S::Cdata)   ? XmlNode::Kind::Cdata
                                          : XmlNode::Kind::ProcessingInstruction;

                bool done = false;
                while (k < got) {
                    const auto* hit = static_cast<const char*>(
                        ::memchr(buf.data() + k, '>', got - k));
                    if (!hit) break;
                    const size_t at = static_cast<size_t>(hit - buf.data());
                    node.append(buf.data() + k, at - k + 1);
                    k = at + 1;
                    if (D::endsWith(node, term)) { done = true; break; }
                }
                if (done) {
                    if (!deliver(kind)) return false;
                    node.clear();
                    state = S::Text;
                    continue;
                }
                if (k < got) { node.append(buf.data() + k, got - k); k = got; }
                continue;
            }

            const char c = buf[k];
            ++k;

            switch (state) {
            case S::Text:
            case S::Comment:
            case S::Cdata:
            case S::Pi:
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
                    if (!deliver(D::classifyTag(node))) return false;
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
