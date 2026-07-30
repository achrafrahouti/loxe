#include "SparseLineIndex.h"
#include "PieceTable.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
// Release page-cache pressure this often during a sequential scan so that
// walking a 2 GB mapping does not park 2 GB of resident pages in our RSS.
constexpr uint64_t kDropPagesEvery = 8ull * 1024 * 1024;
} // namespace

void SparseLineIndex::attach(const PieceTable& doc)
{
    std::unique_lock lock(m_mutex);
    m_doc      = &doc;
    m_docBytes = doc.length();
    m_checkpoints.assign(1, Checkpoint{0, 0});
    m_lastLine = 0;
    m_scanned  = 0;
    m_complete = (m_docBytes == 0);
}

bool SparseLineIndex::build(const PieceTable&        doc,
                            const std::atomic<bool>& cancelled,
                            std::function<void(int)> progress)
{
    {
        std::unique_lock lock(m_mutex);
        m_doc      = &doc;
        m_docBytes = doc.length();
        m_checkpoints.assign(1, Checkpoint{0, 0}); // line 0 starts at byte 0
        m_lastLine = 0;
        m_scanned  = 0;
        m_complete = false;
    }

    const uint64_t total = m_docBytes;
    scan(doc, 0, total, 0, cancelled, progress);

    if (cancelled.load()) return false;

    std::unique_lock lock(m_mutex);
    m_complete = true;
    return true;
}

uint64_t SparseLineIndex::scan(const PieceTable& doc, uint64_t from, uint64_t to,
                               uint64_t startLine, const std::atomic<bool>& cancelled,
                               const std::function<void(int)>& progress)
{
    std::vector<char> buf(kScanChunk);
    uint64_t line       = startLine;
    uint64_t pos        = from;
    uint64_t nextSample = from + kCheckpointInterval;
    uint64_t lastDrop   = from;
    int      lastPct    = -1;

    std::vector<Checkpoint> pending;

    while (pos < to) {
        if (cancelled.load()) break;

        const size_t want = static_cast<size_t>(std::min<uint64_t>(kScanChunk, to - pos));
        const size_t got  = doc.readInto(pos, buf.data(), want);
        if (got == 0) break;

        // memchr over the chunk rather than a call per byte: this is the
        // difference between ~100 MB/s and several GB/s.
        const char* const base = buf.data();
        const char* const end  = base + got;
        const char*       p    = base;
        pending.clear();
        while (p < end) {
            const char* nl = static_cast<const char*>(::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            ++line;
            const uint64_t off = pos + static_cast<uint64_t>(nl - base) + 1;
            if (off >= nextSample) {
                pending.push_back({line, off});
                nextSample = off + kCheckpointInterval;
            }
            p = nl + 1;
        }

        pos += got;

        {
            std::unique_lock lock(m_mutex);
            m_checkpoints.insert(m_checkpoints.end(), pending.begin(), pending.end());
            m_lastLine = line;
            m_scanned  = pos;
        }

        if (pos - lastDrop >= kDropPagesEvery) {
            doc.releasePagesBefore(pos);
            lastDrop = pos;
        }

        if (progress && to > from) {
            const int pct = static_cast<int>((pos - from) * 100 / (to - from));
            if (pct != lastPct) { progress(pct); lastPct = pct; }
        }
    }

    return line;
}

void SparseLineIndex::invalidateFrom(uint64_t byteOffset)
{
    std::unique_lock lock(m_mutex);

    // Keep the checkpoint at or before the edit; everything after it is stale.
    auto it = std::lower_bound(m_checkpoints.begin(), m_checkpoints.end(), byteOffset,
        [](const Checkpoint& cp, uint64_t off) { return cp.offset < off; });
    if (it != m_checkpoints.end())
        m_checkpoints.erase(it, m_checkpoints.end());
    if (m_checkpoints.empty())
        m_checkpoints.push_back({0, 0});

    m_lastLine = m_checkpoints.back().line;
    m_scanned  = m_checkpoints.back().offset;
    m_complete = false;
    m_docBytes = m_doc ? m_doc->length() : 0;
}

bool SparseLineIndex::isComplete() const
{
    std::shared_lock lock(m_mutex);
    return m_complete;
}

uint64_t SparseLineIndex::lineCount() const
{
    uint64_t total = 0;
    bool     done  = false;
    {
        std::shared_lock lock(m_mutex);
        total = m_docBytes;
        done  = m_complete;
    }
    // Force full coverage so the answer is exact rather than extrapolated.
    if (!done) ensureOffsetCovered(total);

    std::shared_lock lock(m_mutex);
    return m_lastLine + 1;
}

uint64_t SparseLineIndex::estimatedLineCount() const
{
    std::shared_lock lock(m_mutex);
    if (m_complete || m_docBytes == 0) return m_lastLine + 1;

    // Extrapolate from the average line length observed so far.
    if (m_scanned == 0) return 1;
    const double perByte = static_cast<double>(m_lastLine) / static_cast<double>(m_scanned);
    return static_cast<uint64_t>(perByte * static_cast<double>(m_docBytes)) + 1;
}

void SparseLineIndex::extendTo(uint64_t targetOffset, uint64_t targetLine) const
{
    std::unique_lock lock(m_mutex);
    if (!m_doc) return;

    uint64_t pos        = m_scanned;
    uint64_t line       = m_lastLine;
    uint64_t nextSample = pos + kCheckpointInterval;
    uint64_t lastDrop    = pos;
    const uint64_t total = m_docBytes;

    if (pos >= total) { m_complete = true; return; }

    std::vector<char> buf(kScanChunk);
    while (pos < total) {
        if (pos >= targetOffset && line >= targetLine) break;

        const size_t want = static_cast<size_t>(std::min<uint64_t>(kScanChunk, total - pos));
        const size_t got  = m_doc->readInto(pos, buf.data(), want);
        if (got == 0) break;

        const char* const base = buf.data();
        const char* const end  = base + got;
        const char*       p    = base;
        while (p < end) {
            const char* nl = static_cast<const char*>(::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            ++line;
            const uint64_t off = pos + static_cast<uint64_t>(nl - base) + 1;
            if (off >= nextSample) {
                m_checkpoints.push_back({line, off});
                nextSample = off + kCheckpointInterval;
            }
            p = nl + 1;
        }

        pos += got;
        m_lastLine = line;
        m_scanned  = pos;

        // The lazy path can cover just as much ground as build(), so it has to
        // release pages behind it too.
        if (pos - lastDrop >= kDropPagesEvery) {
            m_doc->releasePagesBefore(pos);
            lastDrop = pos;
        }
    }

    if (pos >= total) m_complete = true;
}

void SparseLineIndex::ensureLineCovered(uint64_t line) const
{
    {
        std::shared_lock lock(m_mutex);
        if (m_complete || m_lastLine >= line) return;
    }
    extendTo(0, line);
}

void SparseLineIndex::ensureOffsetCovered(uint64_t offset) const
{
    {
        std::shared_lock lock(m_mutex);
        if (m_complete || m_scanned >= offset) return;
    }
    extendTo(offset, 0);
}

uint64_t SparseLineIndex::lineToOffset(uint64_t line) const
{
    if (line == 0) return 0;
    ensureLineCovered(line);

    std::shared_lock lock(m_mutex);
    if (m_checkpoints.empty()) return 0;

    auto it = std::upper_bound(m_checkpoints.begin(), m_checkpoints.end(), line,
        [](uint64_t l, const Checkpoint& cp) { return l < cp.line; });
    if (it != m_checkpoints.begin()) --it;

    uint64_t curLine          = it->line;
    uint64_t curOffset        = it->offset;
    const PieceTable* doc     = m_doc;
    const uint64_t    total   = m_docBytes;
    lock.unlock();

    if (curLine >= line) return curOffset;
    if (!doc || curOffset >= total) return total;

    // Bounded forward scan from the nearest checkpoint (≤ kCheckpointInterval
    // bytes when the index is complete).
    std::vector<char> buf(std::min<uint64_t>(kScanChunk, total - curOffset));
    uint64_t pos = curOffset;
    while (pos < total) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), total - pos));
        const size_t got  = doc->readInto(pos, buf.data(), want);
        if (got == 0) break;

        const char* const base = buf.data();
        const char*       p    = base;
        const char* const end  = base + got;
        while (p < end) {
            const char* nl = static_cast<const char*>(::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            if (++curLine == line)
                return pos + static_cast<uint64_t>(nl - base) + 1;
            p = nl + 1;
        }
        pos += got;
    }
    return total;
}

uint64_t SparseLineIndex::offsetToLine(uint64_t offset) const
{
    if (offset == 0) return 0;
    ensureOffsetCovered(offset);

    std::shared_lock lock(m_mutex);
    if (m_checkpoints.empty()) return 0;

    auto it = std::upper_bound(m_checkpoints.begin(), m_checkpoints.end(), offset,
        [](uint64_t off, const Checkpoint& cp) { return off < cp.offset; });
    if (it != m_checkpoints.begin()) --it;

    uint64_t curLine      = it->line;
    uint64_t curOffset    = it->offset;
    const PieceTable* doc = m_doc;
    lock.unlock();

    if (curOffset >= offset || !doc) return curLine;

    std::vector<char> buf(std::min<uint64_t>(kScanChunk, offset - curOffset));
    uint64_t pos = curOffset;
    while (pos < offset) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), offset - pos));
        const size_t got  = doc->readInto(pos, buf.data(), want);
        if (got == 0) break;

        const char* p   = buf.data();
        const char* end = p + got;
        while (p < end) {
            const char* nl = static_cast<const char*>(::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            ++curLine;
            p = nl + 1;
        }
        pos += got;
    }
    return curLine;
}

uint64_t SparseLineIndex::lineEndOffset(uint64_t line) const
{
    const uint64_t start = lineToOffset(line);

    std::shared_lock lock(m_mutex);
    const PieceTable* doc   = m_doc;
    const uint64_t    total = m_docBytes;
    lock.unlock();

    if (!doc || start >= total) return total;

    std::vector<char> buf(std::min<uint64_t>(kScanChunk, total - start));
    uint64_t pos = start;
    while (pos < total) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), total - pos));
        const size_t got  = doc->readInto(pos, buf.data(), want);
        if (got == 0) break;
        if (const char* nl = static_cast<const char*>(::memchr(buf.data(), '\n', got)))
            return pos + static_cast<uint64_t>(nl - buf.data());
        pos += got;
    }
    return total;
}
