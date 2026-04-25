#include "PieceTable.h"
#include "MmapBuffer.h"

#include <cassert>
#include <mutex>
#include <stdexcept>

PieceTable::PieceTable(const MmapBuffer* file) : m_file(file)
{
    if (file && file->isOpen() && file->size() > 0)
        m_pieces.push_back({PieceOrigin::File, 0, file->size()});
}

uint64_t PieceTable::length() const
{
    std::shared_lock lock(m_mutex);
    uint64_t total = 0;
    for (const auto& p : m_pieces) total += p.length;
    return total;
}

// --- Edit operations ---

void PieceTable::insert(uint64_t pos, std::string_view text)
{
    if (text.empty()) return;
    std::unique_lock lock(m_mutex);

    const uint64_t addOffset = m_addBuffer.size();
    m_addBuffer.append(text.data(), text.size());

    auto [it, intra] = findPiece(pos);

    if (intra == 0) {
        // Insert before current piece
        m_pieces.insert(it, {PieceOrigin::Add, addOffset, text.size()});
    } else if (intra == it->length) {
        // Insert after current piece
        m_pieces.insert(std::next(it), {PieceOrigin::Add, addOffset, text.size()});
    } else {
        // Split current piece
        Piece tail = {it->origin, it->offset + intra, it->length - intra};
        it->length = intra;
        auto next  = std::next(it);
        m_pieces.insert(next, {PieceOrigin::Add, addOffset, text.size()});
        m_pieces.insert(next, tail);
    }

    pushUndo(pos, pos + text.size());
}

void PieceTable::remove(uint64_t pos, uint64_t len)
{
    if (len == 0) return;
    // TODO: split at pos and pos+len, unlink pieces in between
    pushUndo(pos, pos);
}

void PieceTable::replace(uint64_t pos, uint64_t len, std::string_view text)
{
    remove(pos, len);
    insert(pos, text);
}

// --- Undo / Redo ---

bool PieceTable::canUndo() const { return m_undoIndex >= 0; }
bool PieceTable::canRedo() const { return m_undoIndex + 1 < static_cast<int>(m_undoStack.size()); }

void PieceTable::undo(uint64_t* cursorOut)
{
    if (!canUndo()) return;
    std::unique_lock lock(m_mutex);
    const auto& rec = m_undoStack[m_undoIndex--];
    m_pieces = rec.pieces;
    if (cursorOut) *cursorOut = rec.cursorBefore;
}

void PieceTable::redo(uint64_t* cursorOut)
{
    if (!canRedo()) return;
    std::unique_lock lock(m_mutex);
    const auto& rec = m_undoStack[++m_undoIndex];
    m_pieces = rec.pieces;
    if (cursorOut) *cursorOut = rec.cursorAfter;
}

// --- Iterator ---

bool PieceTable::Iterator::atEnd() const
{
    return m_it == m_table->m_pieces.cend();
}

std::string_view PieceTable::Iterator::nextChunk()
{
    if (atEnd()) return {};

    const Piece& p = *m_it;
    const uint64_t remaining = p.length - m_piecePos;

    std::string_view chunk;
    if (p.origin == PieceOrigin::File) {
        chunk = m_table->m_file->slice(
            static_cast<off_t>(p.offset + m_piecePos), static_cast<size_t>(remaining));
    } else {
        chunk = std::string_view(m_table->m_addBuffer).substr(p.offset + m_piecePos, remaining);
    }

    ++m_it;
    m_piecePos = 0;
    return chunk;
}

PieceTable::Iterator PieceTable::begin() const
{
    Iterator it;
    it.m_table    = this;
    it.m_it       = m_pieces.cbegin();
    it.m_piecePos = 0;
    return it;
}

PieceTable::Iterator PieceTable::iteratorAt(uint64_t pos) const
{
    Iterator it;
    it.m_table    = this;
    it.m_piecePos = 0;

    uint64_t accumulated = 0;
    for (auto pit = m_pieces.cbegin(); pit != m_pieces.cend(); ++pit) {
        if (pos <= accumulated + pit->length) {
            it.m_it       = pit;
            it.m_piecePos = pos - accumulated;
            return it;
        }
        accumulated += pit->length;
    }
    it.m_it = m_pieces.cend();
    return it;
}

// --- Private helpers ---

std::pair<std::list<Piece>::iterator, uint64_t> PieceTable::findPiece(uint64_t pos)
{
    uint64_t accumulated = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it) {
        if (pos <= accumulated + it->length)
            return {it, pos - accumulated};
        accumulated += it->length;
    }
    return {m_pieces.end(), 0};
}

void PieceTable::pushUndo(uint64_t cursorBefore, uint64_t cursorAfter)
{
    // Drop redo tail
    if (m_undoIndex + 1 < static_cast<int>(m_undoStack.size()))
        m_undoStack.erase(m_undoStack.begin() + m_undoIndex + 1, m_undoStack.end());

    m_undoStack.push_back({m_pieces, cursorBefore, cursorAfter});
    m_undoIndex = static_cast<int>(m_undoStack.size()) - 1;
}
