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

} // namespace

uint64_t SearchEngine::findForward(const PieceTable& doc, std::string_view needle,
                                   uint64_t from, const Options& opts,
                                   const std::atomic<bool>* cancelled)
{
    if (needle.empty()) return kNotFound;
    const uint64_t total = doc.length();
    if (total < needle.size()) return kNotFound;

    std::string pat(needle);
    if (!opts.caseSensitive) toLowerInPlace(pat.data(), pat.size());

    const size_t overlap = pat.size() - 1;
    std::vector<char> buf(kWindow + overlap);

    auto scanRange = [&](uint64_t begin, uint64_t end) -> uint64_t {
        uint64_t pos = begin;
        while (pos < end) {
            if (isCancelled(cancelled)) return kNotFound;

            // Read an extra `overlap` bytes so a match straddling the window
            // boundary is still contiguous in the buffer.
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(buf.size(), total - pos));
            const size_t got = doc.readInto(pos, buf.data(), want);
            if (got < pat.size()) return kNotFound;

            if (!opts.caseSensitive) toLowerInPlace(buf.data(), got);

            // Only accept matches that *start* before `end`.
            const size_t limit = static_cast<size_t>(
                std::min<uint64_t>(got, (end - pos) + overlap));
            if (const char* hit = findIn(buf.data(), limit, pat.data(), pat.size())) {
                const uint64_t at = pos + static_cast<uint64_t>(hit - buf.data());
                if (at < end) return at;
            }

            if (got < want) return kNotFound;
            pos += kWindow;
        }
        return kNotFound;
    };

    const uint64_t start = std::min(from, total);
    if (const uint64_t hit = scanRange(start, total); hit != kNotFound) return hit;
    if (!opts.wrapAround || start == 0) return kNotFound;

    // Wrap: rescan from the top, allowing a match that ends inside the first
    // region but starts before `start`.
    return scanRange(0, std::min(total, start + needle.size() - 1));
}

uint64_t SearchEngine::findBackward(const PieceTable& doc, std::string_view needle,
                                    uint64_t before, const Options& opts,
                                    const std::atomic<bool>* cancelled)
{
    if (needle.empty()) return kNotFound;
    const uint64_t total = doc.length();
    if (total < needle.size()) return kNotFound;

    // Walk forward through the region and keep the last hit: simpler than a
    // reverse scan and still one pass over the bytes.
    Options fwd = opts;
    fwd.wrapAround = false;

    auto lastIn = [&](uint64_t limit) -> uint64_t {
        uint64_t best = kNotFound;
        uint64_t at   = 0;
        while (true) {
            if (isCancelled(cancelled)) break;
            const uint64_t hit = findForward(doc, needle, at, fwd, cancelled);
            if (hit == kNotFound || hit >= limit) break;
            best = hit;
            at   = hit + 1;
        }
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
    if (needle.empty()) return 0;
    Options fwd = opts;
    fwd.wrapAround = false;

    uint64_t count = 0;
    uint64_t at    = 0;
    while (!isCancelled(cancelled)) {
        const uint64_t hit = findForward(doc, needle, at, fwd, cancelled);
        if (hit == kNotFound) break;
        ++count;
        at = hit + needle.size(); // non-overlapping
    }
    return count;
}
