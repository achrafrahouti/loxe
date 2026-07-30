#include "FormatEngine.h"
#include "PieceTable.h"
#include "XmlScanner.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace {

bool isXmlSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool allSpace(std::string_view s)
{
    for (char c : s)
        if (!isXmlSpace(c)) return false;
    return true;
}

std::string_view trimmed(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && isXmlSpace(s[b])) ++b;
    while (e > b && isXmlSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Re-emits the node stream from XmlScanner with the requested layout.
//
// Beautify puts each element on its own line at its nesting depth, except that
// an element whose only content is text stays on one line (<a>text</a>). Mixed
// content is reflowed, which is standard XML beautifier behaviour but does
// change rendering for whitespace-sensitive formats such as XHTML.
class Layout {
public:
    Layout(const FormatEngine::Options& opts, const FormatEngine::SinkFn& sink)
        : m_opts(opts), m_sink(sink), m_beautify(opts.mode == FormatEngine::Mode::Beautify)
    {
        m_out.reserve(64 * 1024);
    }

    bool node(const XmlNode& n)
    {
        switch (n.kind) {
        case XmlNode::Kind::Text:
            return text(n.raw);

        case XmlNode::Kind::StartTag:
            if (!flushText()) return false;
            markParentHasElement();
            if (!newBlock(m_depth)) return false;
            if (!emit(n.raw)) return false;
            m_stack.push_back({});
            ++m_depth;
            return true;

        case XmlNode::Kind::EmptyTag:
            if (!flushText()) return false;
            markParentHasElement();
            if (!newBlock(m_depth)) return false;
            return emit(n.raw);

        case XmlNode::Kind::EndTag: {
            const bool inlineClose = !m_stack.empty() && !m_stack.back().hasElement;
            if (!flushText()) return false;
            if (m_depth > 0) --m_depth;
            if (!m_stack.empty()) m_stack.pop_back();
            if (m_beautify && !inlineClose) {
                if (!newBlock(m_depth)) return false;
            }
            return emit(n.raw);
        }

        case XmlNode::Kind::Comment:
        case XmlNode::Kind::Cdata:
        case XmlNode::Kind::ProcessingInstruction:
        case XmlNode::Kind::Doctype:
            if (!flushText()) return false;
            markParentHasElement();
            if (!newBlock(m_depth)) return false;
            return emit(n.raw);
        }
        return true;
    }

    bool finish()
    {
        if (!flushText()) return false;
        if (m_beautify && !m_atStart && !emit(m_opts.eol)) return false;
        return flush();
    }

private:
    struct Frame {
        bool hasElement = false; // element children seen at this level
    };

    bool emit(std::string_view s)
    {
        m_out.append(s.data(), s.size());
        if (m_out.size() >= FormatEngine::kOutputBufferSize) return flush();
        return true;
    }

    bool flush()
    {
        if (m_out.empty()) return true;
        const bool ok = m_sink(m_out);
        m_out.clear();
        return ok;
    }

    std::string indent(int depth) const
    {
        if (depth <= 0) return {};
        const auto d = static_cast<size_t>(depth);
        return m_opts.indentStyle == FormatEngine::IndentStyle::Tabs
            ? std::string(d, '\t')
            : std::string(d * static_cast<size_t>(std::max(0, m_opts.indentWidth)), ' ');
    }

    // Starts a node on a fresh line at the given depth.
    bool newBlock(int depth)
    {
        if (!m_beautify) return true;
        if (m_atStart) { m_atStart = false; return emit(indent(depth)); }
        if (!emit(m_opts.eol)) return false;
        return emit(indent(depth));
    }

    void markParentHasElement()
    {
        if (!m_stack.empty()) m_stack.back().hasElement = true;
    }

    bool text(std::string_view raw)
    {
        // XmlScanner splits oversized text nodes, so a chunk arriving while we
        // already hold buffered text is a continuation of the same node and can
        // no longer be treated as collapsible whitespace.
        if (m_textStreaming) return emit(raw);

        if (m_pendingText.size() + raw.size() > kTextBufferCap) {
            if (!commitPendingAsSignificant()) return false;
            m_textStreaming = true;
            return emit(raw);
        }
        m_pendingText.append(raw);
        return true;
    }

    bool commitPendingAsSignificant()
    {
        if (m_pendingText.empty()) return true;
        if (!m_beautify) {
            if (!emit(m_pendingText)) return false;
        } else {
            if (!newBlock(m_depth)) return false;
            if (!emit(trimmed(m_pendingText))) return false;
        }
        m_pendingText.clear();
        markParentHasElement();
        return true;
    }

    // Resolves buffered text just before the next markup node is emitted.
    bool flushText()
    {
        if (m_textStreaming) {
            m_textStreaming = false;
            m_pendingText.clear();
            return true;
        }
        if (m_pendingText.empty()) return true;

        if (!m_beautify) {
            // Minify: drop whitespace-only text between nodes, keep the rest.
            if (!allSpace(m_pendingText) && !emit(m_pendingText)) return false;
            m_pendingText.clear();
            return true;
        }

        if (allSpace(m_pendingText)) { m_pendingText.clear(); return true; }

        // Significant text. When the enclosing element has no element children
        // yet, keep it on the same line as the start tag.
        if (!m_stack.empty() && !m_stack.back().hasElement) {
            if (!emit(trimmed(m_pendingText))) return false;
        } else {
            if (!newBlock(m_depth)) return false;
            if (!emit(trimmed(m_pendingText))) return false;
        }
        m_pendingText.clear();
        return true;
    }

    // Buffered text beyond this is certainly significant, so stop buffering.
    static constexpr size_t kTextBufferCap = 1024 * 1024;

    const FormatEngine::Options& m_opts;
    const FormatEngine::SinkFn&  m_sink;
    const bool                   m_beautify;

    std::string        m_out;
    std::string        m_pendingText;
    std::vector<Frame> m_stack;
    int                m_depth         = 0;
    bool               m_atStart       = true;
    bool               m_textStreaming = false;
};

} // namespace

bool FormatEngine::formatToSink(const PieceTable& src,
                                const Options&    opts,
                                const SinkFn&     sink,
                                ProgressFn        progress,
                                CancelFn          cancelled)
{
    if (!sink) return false;

    const uint64_t total = src.length();
    Layout layout(opts, sink);

    auto lastTick = std::chrono::steady_clock::now();
    int  lastPct  = -1;
    bool aborted  = false;

    std::atomic<bool> cancelFlag{false};

    const bool completed = XmlScanner::scanAll(src, [&](const XmlNode& n) {
        if (cancelled && cancelled()) { cancelFlag.store(true); return false; }
        if (!layout.node(n)) { aborted = true; return false; }

        // Progress no more often than every ~100 ms.
        if (progress && total > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTick >= std::chrono::milliseconds(100)) {
                const int pct = static_cast<int>((n.offset + n.raw.size()) * 100 / total);
                if (pct != lastPct) { progress(pct); lastPct = pct; }
                lastTick = now;
            }
        }
        return true;
    }, &cancelFlag);

    if (!completed || aborted) return false;
    if (cancelled && cancelled()) return false;
    if (!layout.finish()) return false;
    if (progress) progress(100);
    return true;
}

std::unique_ptr<PieceTable> FormatEngine::format(
    const PieceTable& src,
    const Options&    opts,
    ProgressFn        progress,
    CancelFn          cancelled)
{
    auto out = std::make_unique<PieceTable>(nullptr);

    const bool ok = formatToSink(src, opts,
        [&out](std::string_view block) {
            out->appendInitial(block);
            return true;
        },
        std::move(progress), std::move(cancelled));

    if (!ok) return nullptr;
    return out;
}
