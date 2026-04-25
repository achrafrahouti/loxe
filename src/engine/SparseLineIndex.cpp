#include "SparseLineIndex.h"
#include "MmapBuffer.h"

#include <algorithm>
#include <cassert>
#include <mutex>

bool SparseLineIndex::build(const MmapBuffer& buf, const std::atomic<bool>& cancelled)
{
    std::unique_lock lock(m_mutex);
    m_checkpoints.clear();
    m_totalLines = 0;
    m_totalBytes = buf.size();
    m_complete   = false;
    m_buf        = &buf;
    lock.unlock();

    // Checkpoint at byte 0 / line 0
    {
        std::unique_lock l(m_mutex);
        m_checkpoints.push_back({0, 0});
    }

    scan(buf, 0, buf.size(), 0, cancelled);

    if (cancelled.load()) return false;

    std::unique_lock l(m_mutex);
    m_complete = true;
    return true;
}

void SparseLineIndex::scan(const MmapBuffer& buf, uint64_t from, uint64_t to,
                            uint64_t startLine, const std::atomic<bool>& cancelled)
{
    uint64_t line       = startLine;
    uint64_t nextSample = from + kCheckpointInterval;

    for (uint64_t i = from; i < to; ++i) {
        if (cancelled.load()) return;

        const char c = buf.slice(static_cast<off_t>(i), 1)[0];
        if (c == '\n') {
            ++line;
            if (i + 1 >= nextSample) {
                std::unique_lock l(m_mutex);
                m_checkpoints.push_back({line, i + 1});
                m_totalLines = line;
                nextSample = i + 1 + kCheckpointInterval;
            }
        }
    }

    std::unique_lock l(m_mutex);
    m_totalLines = line;
}

void SparseLineIndex::invalidateFrom(uint64_t byteOffset)
{
    std::unique_lock lock(m_mutex);
    auto it = std::lower_bound(m_checkpoints.begin(), m_checkpoints.end(), byteOffset,
        [](const Checkpoint& cp, uint64_t off) { return cp.offset < off; });
    if (it != m_checkpoints.end())
        m_checkpoints.erase(it, m_checkpoints.end());
    m_complete = false;
}

bool SparseLineIndex::isComplete() const
{
    std::shared_lock lock(m_mutex);
    return m_complete;
}

uint64_t SparseLineIndex::lineCount() const
{
    std::shared_lock lock(m_mutex);
    return m_totalLines;
}

uint64_t SparseLineIndex::estimatedLineCount() const
{
    std::shared_lock lock(m_mutex);
    if (m_complete || m_totalBytes == 0) return m_totalLines;

    // Extrapolate from checkpoints scanned so far
    if (m_checkpoints.empty()) return 0;
    const auto& last = m_checkpoints.back();
    if (last.offset == 0) return 0;
    return static_cast<uint64_t>(
        static_cast<double>(last.line) / last.offset * m_totalBytes);
}

uint64_t SparseLineIndex::lineToOffset(uint64_t line) const
{
    std::shared_lock lock(m_mutex);
    if (m_checkpoints.empty()) return 0;

    auto it = std::upper_bound(m_checkpoints.begin(), m_checkpoints.end(), line,
        [](uint64_t l, const Checkpoint& cp) { return l < cp.line; });
    if (it != m_checkpoints.begin()) --it;

    uint64_t curLine   = it->line;
    uint64_t curOffset = it->offset;
    const MmapBuffer* buf   = m_buf;
    const uint64_t    total = m_totalBytes;
    lock.unlock();

    if (curLine == line) return curOffset;
    if (!buf || !buf->isOpen() || curOffset >= total) return curOffset;

    const std::string_view segment = buf->slice(static_cast<off_t>(curOffset),
                                                 static_cast<size_t>(total - curOffset));
    for (size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == '\n') {
            ++curLine;
            if (curLine == line) return curOffset + i + 1;
        }
    }
    return total;
}

uint64_t SparseLineIndex::offsetToLine(uint64_t offset) const
{
    std::shared_lock lock(m_mutex);
    if (m_checkpoints.empty()) return 0;

    auto it = std::upper_bound(m_checkpoints.begin(), m_checkpoints.end(), offset,
        [](uint64_t off, const Checkpoint& cp) { return off < cp.offset; });
    if (it != m_checkpoints.begin()) --it;

    uint64_t curLine   = it->line;
    uint64_t curOffset = it->offset;
    const MmapBuffer* buf = m_buf;
    lock.unlock();

    if (curOffset >= offset || !buf || !buf->isOpen()) return curLine;

    const std::string_view segment = buf->slice(static_cast<off_t>(curOffset),
                                                 static_cast<size_t>(offset - curOffset));
    for (char c : segment)
        if (c == '\n') ++curLine;
    return curLine;
}
