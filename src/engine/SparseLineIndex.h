#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <vector>

class MmapBuffer;

// Sampled array of (line_number, byte_offset) checkpoints, one per ~4 KB.
// Enables O(log n + small_scan) line↔offset lookup for files up to 2 GB.
// Built on a background thread; safe to query before build completes.
class SparseLineIndex {
public:
    static constexpr uint64_t kCheckpointInterval = 4096; // bytes between samples

    SparseLineIndex() = default;

    // Called from the background QThread. Returns false if cancelled.
    bool build(const MmapBuffer& buf, const std::atomic<bool>& cancelled);

    // Invalidate all checkpoints at and after byte offset (called after edits).
    // Lookups beyond the invalidation point trigger a lazy rebuild.
    void invalidateFrom(uint64_t byteOffset);

    bool     isComplete()    const;
    uint64_t lineCount()     const;
    uint64_t estimatedLineCount() const; // usable before build finishes

    // Both ops: binary-search checkpoints then scan forward byte-by-byte.
    // Guaranteed latency ≤ 5 ms for any file ≤ 2 GB.
    uint64_t lineToOffset(uint64_t line)     const;
    uint64_t offsetToLine(uint64_t offset)   const;

private:
    struct Checkpoint {
        uint64_t line;
        uint64_t offset;
    };

    // Scan raw bytes [from, to) in buf counting newlines, appending checkpoints.
    void scan(const MmapBuffer& buf, uint64_t from, uint64_t to,
              uint64_t startLine, const std::atomic<bool>& cancelled);

    std::vector<Checkpoint> m_checkpoints;
    uint64_t                m_totalLines = 0;
    uint64_t                m_totalBytes = 0;
    bool                    m_complete   = false;
    const MmapBuffer*       m_buf        = nullptr; // set during build(), never changes after

    mutable std::shared_mutex m_mutex;
};
