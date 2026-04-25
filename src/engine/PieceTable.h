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
    explicit PieceTable(const MmapBuffer* file);

    uint64_t length() const;

    // --- Edit operations (each pushes an undo record) ---
    void insert(uint64_t pos, std::string_view text);
    void remove(uint64_t pos, uint64_t len);
    void replace(uint64_t pos, uint64_t len, std::string_view text);

    // --- Undo / Redo ---
    bool canUndo() const;
    bool canRedo() const;
    void undo(uint64_t* cursorOut = nullptr);
    void redo(uint64_t* cursorOut = nullptr);

    // --- Sequential iterator (used by FormatEngine and search) ---
    class Iterator {
    public:
        bool             atEnd() const;
        std::string_view nextChunk(); // zero-copy span; valid until next call

    private:
        friend class PieceTable;
        const PieceTable*                 m_table    = nullptr;
        std::list<Piece>::const_iterator  m_it;
        uint64_t                          m_piecePos = 0; // bytes consumed in current piece
    };

    Iterator begin() const;
    Iterator iteratorAt(uint64_t pos) const; // iterator positioned at byte offset pos

private:
    struct UndoRecord {
        std::list<Piece> pieces;
        uint64_t         cursorBefore = 0;
        uint64_t         cursorAfter  = 0;
    };

    // Locates the piece containing document position pos.
    // Returns iterator and intra-piece byte offset.
    std::pair<std::list<Piece>::iterator, uint64_t> findPiece(uint64_t pos);

    void pushUndo(uint64_t cursorBefore, uint64_t cursorAfter);

    const MmapBuffer*   m_file;
    std::string         m_addBuffer;
    std::list<Piece>    m_pieces;

    std::vector<UndoRecord> m_undoStack;
    int                     m_undoIndex = -1;

    mutable std::shared_mutex m_mutex;
};
