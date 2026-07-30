#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

class MmapBuffer;

enum class PieceOrigin : uint8_t { File, Add };

struct Piece {
    PieceOrigin origin;
    uint64_t    offset; // byte offset into FILE buffer or ADD buffer
    uint64_t    length;
};

// Document model: a doubly-linked list of Pieces referencing two buffers:
//   FILE — the read-only memory-mapped original file (MmapBuffer)
//   ADD  — an append-only in-memory string of typed characters
//
// Thread safety: one writer (UI thread) + multiple readers via std::shared_mutex.
class PieceTable {
public:
    // Largest span returned by a single chunkAt()/nextChunk() call. Bounding it
    // keeps the pread fallback from materialising gigabyte scratch buffers.
    static constexpr size_t kMaxChunk = 1024 * 1024;

    // Undo history is dropped oldest-first once the retained piece lists exceed
    // this much memory (SRS: 512 MB default cap).
    static constexpr uint64_t kUndoMemoryCap = 512ull * 1024 * 1024;

    explicit PieceTable(const MmapBuffer* file);

    uint64_t length() const;

    // --- Edit operations (each pushes an undo record) ---
    void insert(uint64_t pos, std::string_view text);
    void remove(uint64_t pos, uint64_t len);
    void replace(uint64_t pos, uint64_t len, std::string_view text);

    // Replace the whole document as a single undo step.
    void replaceAll(std::string_view text);

    // Append without recording undo. For building a fresh document only
    // (new file, FormatEngine output) — never for user edits.
    void appendInitial(std::string_view text);

    // --- Undo / Redo ---
    //
    // Compound operations (beautify, replace-all, encoding change) wrap
    // themselves in begin/endUndoGroup so they undo in one step. Groups nest.
    void beginUndoGroup();
    void endUndoGroup();

    bool     canUndo() const;
    bool     canRedo() const;
    void     undo(uint64_t* cursorOut = nullptr);
    void     redo(uint64_t* cursorOut = nullptr);
    void     clearUndo();
    uint64_t undoMemoryBytes() const;
    // True once history has been discarded to stay under kUndoMemoryCap.
    bool     undoTruncated() const;

    // --- Reading ---

    // Copies up to len bytes starting at pos into dst. Returns bytes copied,
    // which is short only at end of document. Safe to call concurrently.
    size_t readInto(uint64_t pos, char* dst, size_t len) const;

    // Convenience copy of [pos, pos+len). Caller controls the size.
    std::string read(uint64_t pos, uint64_t len) const;

    // Tells the OS that file-backed bytes before `offset` are no longer needed,
    // so their pages can be dropped. Any sequential pass over a large document
    // must call this periodically: mapped pages count toward RSS while resident,
    // so a full 2 GB scan would otherwise blow the 80 MB budget. A later read of
    // the same range simply faults it back in.
    void releasePagesBefore(uint64_t offset) const;

    // Zero-copy view of at most min(maxLen, kMaxChunk) bytes at pos. May be
    // short at a piece boundary; empty at end of document. The view aliases the
    // FILE mapping or the ADD buffer, so it is invalidated by any edit — use
    // readInto() from background threads that race with the writer.
    std::string_view chunkAt(uint64_t pos, size_t maxLen) const;

    // --- Sequential iterator (used by FormatEngine and search) ---
    class Iterator {
    public:
        bool             atEnd() const;
        std::string_view nextChunk(); // zero-copy span; valid until next call
        uint64_t         offset() const { return m_docPos; }

    private:
        friend class PieceTable;
        const PieceTable*                m_table    = nullptr;
        std::list<Piece>::const_iterator m_it;
        uint64_t                         m_piecePos = 0; // bytes consumed in current piece
        uint64_t                         m_docPos   = 0; // absolute document offset
    };

    Iterator begin() const;
    Iterator iteratorAt(uint64_t pos) const; // iterator positioned at byte offset pos

    size_t pieceCount() const;

private:
    struct UndoRecord {
        std::list<Piece> before;       // piece list prior to the edit
        std::list<Piece> after;        // piece list once the edit was applied
        uint64_t         cursorBefore = 0;
        uint64_t         cursorAfter  = 0;
    };

    // All *Locked helpers assume the caller holds m_mutex.
    uint64_t lengthLocked() const;

    // Piece containing document position `pos`, with its start offset in
    // *accOut. Resumes from a per-thread cursor when possible, so a sequential
    // pass does not re-walk the list on every call. Caller must hold m_mutex.
    std::list<Piece>::const_iterator seek(uint64_t pos, uint64_t* accOut) const;

    // Stamp identifying one immutable state of the piece list.
    static uint64_t nextGeneration();

    // Locates the piece containing document position pos.
    // Returns {end(), 0} when pos is at (or past) the end of the document.
    std::pair<std::list<Piece>::iterator, uint64_t> findPiece(uint64_t pos);

    // Splits the piece at pos so that a piece boundary falls exactly on pos,
    // returning an iterator to the piece starting at pos (may be end()).
    std::list<Piece>::iterator splitAt(uint64_t pos);

    void pushUndo(std::list<Piece> before, uint64_t cursorBefore, uint64_t cursorAfter);
    void trimUndoToCap();

    const MmapBuffer* m_file;
    std::string       m_addBuffer;
    std::list<Piece>  m_pieces;
    // Sum of the piece lengths, maintained incrementally. length() is called
    // once per read on every streaming pass, so it must not walk the list.
    uint64_t          m_length = 0;
    // Bumped by every mutation; invalidates cached piece iterators.
    uint64_t          m_generation = nextGeneration();

    std::vector<UndoRecord> m_undoStack;
    int                     m_undoIndex     = -1;
    bool                    m_undoTruncated = false;

    // Undo-group state.
    int              m_groupDepth = 0;
    std::list<Piece> m_groupBefore;
    uint64_t         m_groupCursorBefore = 0;
    bool             m_groupDirty        = false;

    mutable std::shared_mutex m_mutex;
};
