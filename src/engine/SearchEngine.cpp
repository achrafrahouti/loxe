#include "SearchEngine.h"
#include "PieceTable.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace {

void toLowerInPlace(char* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        p[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(p[i])));
}

// Locate needle within [hay, hay+hayLen). Uses memmem() on glibc — its SIMD
// implementation is what carries the plain-text throughput target.
const char* findIn(const char* hay, size_t hayLen, const char* needle, size_t needleLen)
{
    if (needleLen == 0 || hayLen < needleLen) return nullptr;
#if defined(__GLIBC__)
    return static_cast<const char*>(::memmem(hay, hayLen, needle, needleLen));
#else
    const char* const end = hay + hayLen - needleLen + 1;
    for (const char* p = hay; p < end; ) {
        p = static_cast<const char*>(std::memchr(p, needle[0], static_cast<size_t>(end - p)));
        if (!p) return nullptr;
        if (std::memcmp(p, needle, needleLen) == 0) return p;
        ++p;
    }
    return nullptr;
#endif
}

bool isCancelled(const std::atomic<bool>* flag)
{
    return flag && flag->load();
}

// One pass over [begin, end) reporting every match start through `cb`.
//
// The window slides forward exactly once over the range; each byte is read and
// compared once. Every caller that wants more than the first match goes through
// here — repeatedly restarting findForward() from the previous hit re-read a
// whole window per match, which made Replace All and countAll quadratic in the
// number of matches (2.8 MB/s on a match-dense document, versus > 1 GB/s here).
//
// `advance` is how far past a hit the next search resumes: needle.size() for
// non-overlapping matches, 1 to allow overlaps.
void scanMatches(const PieceTable& doc, const std::string& pat, bool caseSensitive,
                 uint64_t begin, uint64_t end, size_t advance,
                 const std::atomic<bool>* cancelled,
                 const SearchEngine::MatchFn& cb)
{
    if (pat.empty() || begin >= end) return;

    const size_t overlap = pat.size() - 1;
    const uint64_t total = doc.length();
    std::vector<char> buf(SearchEngine::kWindow + overlap);

    uint64_t pos      = begin;
    uint64_t resumeAt = begin; // absolute offset the next search may start at

    while (pos < end) {
        if (isCancelled(cancelled)) return;

        // Read an extra `overlap` bytes so a match straddling the window
        // boundary is still contiguous in the buffer.
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(buf.size(), total > pos ? total - pos : 0));
        const size_t got = doc.readInto(pos, buf.data(), want);
        if (got < pat.size()) return;

        if (!caseSensitive) toLowerInPlace(buf.data(), got);

        // Only accept matches that *start* before `end`: the trailing `overlap`
        // bytes exist to complete a match, not to begin one.
        const size_t limit = static_cast<size_t>(
            std::min<uint64_t>(got, (end - pos) + overlap));

        size_t off = (resumeAt > pos) ? static_cast<size_t>(resumeAt - pos) : 0;
        while (off + pat.size() <= limit) {
            const char* hit = findIn(buf.data() + off, limit - off, pat.data(), pat.size());
            if (!hit) break;

            const size_t idx = static_cast<size_t>(hit - buf.data());
            const uint64_t at = pos + idx;
            if (at >= end) return;
            if (!cb(at)) return;

            off      = idx + advance;
            resumeAt = at + advance;
        }

        if (got < want) return;

        // Every match starting in [pos, pos + kWindow) was reachable in this
        // buffer, so the next window begins exactly where this one left off.
        pos += SearchEngine::kWindow;
    }
}

std::string preparePattern(std::string_view needle, bool caseSensitive)
{
    std::string pat(needle);
    if (!caseSensitive) toLowerInPlace(pat.data(), pat.size());
    return pat;
}

} // namespace

void SearchEngine::forEachMatch(const PieceTable& doc, std::string_view needle,
                                uint64_t from, const Options& opts,
                                const std::atomic<bool>* cancelled,
                                const MatchFn& cb)
{
    if (needle.empty() || !cb) return;
    const uint64_t total = doc.length();
    if (total < needle.size()) return;

    const std::string pat = preparePattern(needle, opts.caseSensitive);
    scanMatches(doc, pat, opts.caseSensitive, std::min(from, total), total,
                pat.size(), cancelled, cb);
}

uint64_t SearchEngine::findForward(const PieceTable& doc, std::string_view needle,
                                   uint64_t from, const Options& opts,
                                   const std::atomic<bool>* cancelled)
{
    if (needle.empty()) return kNotFound;
    const uint64_t total = doc.length();
    if (total < needle.size()) return kNotFound;

    const std::string pat = preparePattern(needle, opts.caseSensitive);

    uint64_t found = kNotFound;
    auto first = [&found](uint64_t at) { found = at; return false; };

    const uint64_t start = std::min(from, total);
    scanMatches(doc, pat, opts.caseSensitive, start, total, pat.size(), cancelled, first);
    if (found != kNotFound) return found;
    if (!opts.wrapAround || start == 0) return kNotFound;

    // Wrap: rescan from the top, allowing a match that ends inside the first
    // region but starts before `start`.
    scanMatches(doc, pat, opts.caseSensitive, 0,
                std::min(total, start + needle.size() - 1), pat.size(), cancelled, first);
    return found;
}

uint64_t SearchEngine::findBackward(const PieceTable& doc, std::string_view needle,
                                    uint64_t before, const Options& opts,
                                    const std::atomic<bool>* cancelled)
{
    if (needle.empty()) return kNotFound;
    const uint64_t total = doc.length();
    if (total < needle.size()) return kNotFound;

    const std::string pat = preparePattern(needle, opts.caseSensitive);

    // Walk forward through the region and keep the last hit: simpler than a
    // reverse scan and still one pass over the bytes. Overlapping matches count,
    // so the step is 1 rather than the needle length.
    auto lastIn = [&](uint64_t limit) -> uint64_t {
        uint64_t best = kNotFound;
        scanMatches(doc, pat, opts.caseSensitive, 0, limit, 1, cancelled,
                    [&best](uint64_t at) { best = at; return true; });
        return best;
    };

    const uint64_t limit = std::min(before, total);
    if (const uint64_t hit = lastIn(limit); hit != kNotFound) return hit;
    if (!opts.wrapAround) return kNotFound;
    return lastIn(total);
}

uint64_t SearchEngine::countAll(const PieceTable& doc, std::string_view needle,
                                const Options& opts,
                                const std::atomic<bool>* cancelled)
{
    uint64_t count = 0;
    forEachMatch(doc, needle, 0, opts, cancelled,
                 [&count](uint64_t) { ++count; return true; });
    return count;
}
