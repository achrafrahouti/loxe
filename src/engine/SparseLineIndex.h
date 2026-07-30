#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <vector>

class PieceTable;

// Sampled array of (line_number, byte_offset) checkpoints, one per ~4 KB.
// Enables O(log n + small_scan) line↔offset lookup for files up to 2 GB.
//
// The index is built over the PieceTable, not the raw file, so lookups stay
// correct after edits. Built on a background thread; safe to query before the
// build completes and safe to query concurrently with it.
class SparseLineIndex {
public:
    static constexpr uint64_t kCheckpointInterval = 4096;        // bytes between samples
    static constexpr size_t   kScanChunk          = 256 * 1024;  // scan granularity

    SparseLineIndex() = default;

    // Called from the background QThread. Returns false if cancelled.
    // progress (optional) receives [0, 100] as the scan advances.
    bool build(const PieceTable&        doc,
               const std::atomic<bool>& cancelled,
               std::function<void(int)> progress = {});

    // Attaches the index to a document without scanning it. Lookups then
    // extend the checkpoint array lazily, on demand. Used for documents created
    // in-memory (File > New) and to make the viewport usable during phase 1.
    void attach(const PieceTable& doc);

    // Invalidate all checkpoints at and after byte offset (called after edits).
    // Lookups beyond the invalidation point trigger a bounded lazy rebuild.
    void invalidateFrom(uint64_t byteOffset);

    bool     isComplete() const;
    // Total line count: newline count + 1. An empty document has one line.
    uint64_t lineCount() const;
    uint64_t estimatedLineCount() const; // usable before build finishes

    // Both ops: binary-search checkpoints, then scan forward in bounded chunks,
    // extending the checkpoint array as they go so repeat lookups are cheap.
    uint64_t lineToOffset(uint64_t line)   const;
    uint64_t offsetToLine(uint64_t offset) const;

    // Byte offset one past the last content byte of `line`, i.e. the position of
    // its terminating newline (or end of document on the final line).
    uint64_t lineEndOffset(uint64_t line) const;

private:
    struct Checkpoint {
        uint64_t line;
        uint64_t offset;
    };

    // Scans [from, to) counting newlines and appending checkpoints.
    // Returns the line number reached, or leaves early when cancelled.
    uint64_t scan(const PieceTable& doc, uint64_t from, uint64_t to,
                  uint64_t startLine, const std::atomic<bool>& cancelled,
                  const std::function<void(int)>& progress);

    // Extends checkpoints forward from the last one until `line` is covered (or
    // EOF). Caller must NOT hold m_mutex. Never rescans from byte 0.
    void ensureLineCovered(uint64_t line) const;
    void ensureOffsetCovered(uint64_t offset) const;
    void extendTo(uint64_t targetOffset, uint64_t targetLine) const;

    mutable std::vector<Checkpoint> m_checkpoints;
    mutable uint64_t                m_lastLine  = 0; // newline count seen so far
    mutable uint64_t                m_scanned   = 0; // bytes covered by checkpoints
    mutable bool                    m_complete  = false;
    uint64_t                        m_docBytes  = 0;
    const PieceTable*               m_doc       = nullptr;

    mutable std::shared_mutex m_mutex;
};
