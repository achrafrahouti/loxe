#include "PieceTable.h"
#include "MmapBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace {

// Retained-memory estimate for one piece list: the Piece plus the two
// std::list node pointers that hold it.
constexpr uint64_t kBytesPerPiece = sizeof(Piece) + 2 * sizeof(void*);

uint64_t listBytes(const std::list<Piece>& l)
{
    return static_cast<uint64_t>(l.size()) * kBytesPerPiece;
}

// Globally unique, monotonically increasing. A cached iterator is only reused
// when the generation still matches, and because the counter is never reused
// no other PieceTable — including one allocated at a recycled address — can
// validate a stale cache.
std::atomic<uint64_t> g_generation{1};

} // namespace

uint64_t PieceTable::nextGeneration()
{
    return g_generation.fetch_add(1, std::memory_order_relaxed);
}

PieceTable::PieceTable(const MmapBuffer* file) : m_file(file)
{
    if (file && file->isOpen() && file->size() > 0) {
        m_pieces.push_back({PieceOrigin::File, 0, file->size()});
        m_length = file->size();
    }
}

uint64_t PieceTable::length() const
{
    std::shared_lock lock(m_mutex);
    return m_length;
}

// Only for recovering the cached length after the piece list is replaced
// wholesale (undo/redo). Every other path adjusts m_length incrementally —
// walking the list per edit made a long Replace All quadratic on its own.
uint64_t PieceTable::lengthLocked() const
{
    uint64_t total = 0;
    for (const auto& p : m_pieces) total += p.length;
    return total;
}

size_t PieceTable::pieceCount() const
{
    std::shared_lock lock(m_mutex);
    return m_pieces.size();
}

// --- Edit operations ---

void PieceTable::insert(uint64_t pos, std::string_view text)
{
    if (text.empty()) return;
    std::unique_lock lock(m_mutex);

    const uint64_t total = m_length;
    if (pos > total) pos = total;

    // Inside an undo group the group's own pre-edit snapshot is what gets
    // recorded, so copying the whole piece list per edit is pure waste — and
    // with the list growing by two pieces per edit, quadratic waste.
    std::list<Piece> before;
    if (m_groupDepth == 0) before = m_pieces;

    m_length += text.size();
    m_generation = nextGeneration();

    const uint64_t addOffset = m_addBuffer.size();
    m_addBuffer.append(text.data(), text.size());
    const Piece np{PieceOrigin::Add, addOffset, text.size()};

    auto [it, intra] = findPiece(pos);

    if (it == m_pieces.end()) {
        // Appending at end of document. Consecutive typing lands in adjacent
        // ADD-buffer bytes, so extend the trailing piece instead of growing the
        // list by one piece per keystroke.
        if (!m_pieces.empty()) {
            Piece& back = m_pieces.back();
            if (back.origin == PieceOrigin::Add && back.offset + back.length == addOffset) {
                back.length += text.size();
                pushUndo(std::move(before), pos, pos + text.size());
                return;
            }
        }
        m_pieces.push_back(np);
    } else if (intra == 0) {
        // Insert before the piece — coalesce with the preceding ADD piece when
        // it is contiguous in the ADD buffer.
        if (it != m_pieces.begin()) {
            auto prev = std::prev(it);
            if (prev->origin == PieceOrigin::Add && prev->offset + prev->length == addOffset) {
                prev->length += text.size();
                pushUndo(std::move(before), pos, pos + text.size());
                return;
            }
        }
        m_pieces.insert(it, np);
    } else {
        // Split the piece and land the new text in the gap.
        const Piece tail{it->origin, it->offset + intra, it->length - intra};
        it->length = intra;
        auto next  = std::next(it);
        m_pieces.insert(next, np);
        m_pieces.insert(next, tail);
    }

    pushUndo(std::move(before), pos, pos + text.size());
}

void PieceTable::remove(uint64_t pos, uint64_t len)
{
    if (len == 0) return;
    std::unique_lock lock(m_mutex);

    const uint64_t total = m_length;
    if (pos >= total) return;
    len = std::min(len, total - pos);

    std::list<Piece> before;
    if (m_groupDepth == 0) before = m_pieces;

    m_length -= len;
    m_generation = nextGeneration();

    // Put a piece boundary exactly on pos, then drop whole pieces until len
    // bytes are gone, trimming the final partially-covered piece.
    auto it = splitAt(pos);
    uint64_t remaining = len;
    while (remaining > 0 && it != m_pieces.end()) {
        if (it->length <= remaining) {
            remaining -= it->length;
            it = m_pieces.erase(it);
        } else {
            it->offset += remaining;
            it->length -= remaining;
            remaining = 0;
        }
    }

    pushUndo(std::move(before), pos + len, pos);
}

void PieceTable::replace(uint64_t pos, uint64_t len, std::string_view text)
{
    beginUndoGroup();
    remove(pos, len);
    insert(pos, text);
    endUndoGroup();
}

void PieceTable::replaceAll(std::string_view text)
{
    beginUndoGroup();
    remove(0, length());
    insert(0, text);
    endUndoGroup();
}

void PieceTable::appendInitial(std::string_view text)
{
    if (text.empty()) return;
    std::unique_lock lock(m_mutex);

    const uint64_t addOffset = m_addBuffer.size();
    m_addBuffer.append(text.data(), text.size());
    m_length += text.size();
    m_generation = nextGeneration();

    if (!m_pieces.empty()) {
        Piece& back = m_pieces.back();
        if (back.origin == PieceOrigin::Add && back.offset + back.length == addOffset) {
            back.length += text.size();
            return;
        }
    }
    m_pieces.push_back({PieceOrigin::Add, addOffset, text.size()});
}

// --- Undo / Redo ---

void PieceTable::beginUndoGroup()
{
    std::unique_lock lock(m_mutex);
    if (m_groupDepth++ == 0) {
        m_groupBefore       = m_pieces;
        m_groupCursorBefore = 0;
        m_groupDirty        = false;
    }
}

void PieceTable::endUndoGroup()
{
    std::unique_lock lock(m_mutex);
    if (m_groupDepth == 0) return;
    if (--m_groupDepth > 0) return;
    if (!m_groupDirty) return; // nothing was actually edited

    // Collapse everything the group did into one record.
    m_undoStack.push_back({std::move(m_groupBefore), m_pieces,
                           m_groupCursorBefore, m_groupCursorBefore});
    m_undoIndex = static_cast<int>(m_undoStack.size()) - 1;
    m_groupBefore.clear();
    m_groupDirty = false;
    trimUndoToCap();
}

bool PieceTable::canUndo() const
{
    std::shared_lock lock(m_mutex);
    return m_undoIndex >= 0;
}

bool PieceTable::canRedo() const
{
    std::shared_lock lock(m_mutex);
    return m_undoIndex + 1 < static_cast<int>(m_undoStack.size());
}

void PieceTable::undo(uint64_t* cursorOut)
{
    std::unique_lock lock(m_mutex);
    if (m_undoIndex < 0) return;
    const UndoRecord& rec = m_undoStack[static_cast<size_t>(m_undoIndex)];
    m_pieces = rec.before;
    m_length     = lengthLocked(); // the list was replaced wholesale
    m_generation = nextGeneration();
    if (cursorOut) *cursorOut = rec.cursorBefore;
    --m_undoIndex;
}

void PieceTable::redo(uint64_t* cursorOut)
{
    std::unique_lock lock(m_mutex);
    if (m_undoIndex + 1 >= static_cast<int>(m_undoStack.size())) return;
    const UndoRecord& rec = m_undoStack[static_cast<size_t>(++m_undoIndex)];
    m_pieces     = rec.after;
    m_length     = lengthLocked();
    m_generation = nextGeneration();
    if (cursorOut) *cursorOut = rec.cursorAfter;
}

void PieceTable::clearUndo()
{
    std::unique_lock lock(m_mutex);
    m_undoStack.clear();
    m_undoIndex     = -1;
    m_undoTruncated = false;
}

uint64_t PieceTable::undoMemoryBytes() const
{
    std::shared_lock lock(m_mutex);
    uint64_t total = 0;
    for (const auto& rec : m_undoStack)
        total += listBytes(rec.before) + listBytes(rec.after);
    return total;
}

bool PieceTable::undoTruncated() const
{
    std::shared_lock lock(m_mutex);
    return m_undoTruncated;
}

// --- Reading ---

// Locates the piece containing `pos`, resuming from this thread's last lookup
// when that lands at or before `pos`. Every reader here is a sequential pass, so
// without the cursor each call re-walks the list from the head — after a large
// Replace All leaves millions of pieces behind, that alone takes a streaming
// read from GB/s down to single-digit MB/s.
//
// The cursor is thread_local, so concurrent readers under the shared lock never
// touch each other's copy, and it is invalidated by the generation stamp that
// every mutation bumps.
std::list<Piece>::const_iterator PieceTable::seek(uint64_t pos, uint64_t* accOut) const
{
    struct Cursor {
        const PieceTable*                table = nullptr;
        uint64_t                         gen   = 0;
        std::list<Piece>::const_iterator it;
        uint64_t                         acc   = 0;
    };
    static thread_local Cursor cursor;

    auto     it  = m_pieces.cbegin();
    uint64_t acc = 0;
    if (cursor.table == this && cursor.gen == m_generation && cursor.acc <= pos) {
        it  = cursor.it;
        acc = cursor.acc;
    }

    while (it != m_pieces.cend() && pos >= acc + it->length) {
        acc += it->length;
        ++it;
    }

    if (it != m_pieces.cend())
        cursor = Cursor{this, m_generation, it, acc};

    *accOut = acc;
    return it;
}

size_t PieceTable::readInto(uint64_t pos, char* dst, size_t len) const
{
    if (!dst || len == 0) return 0;
    std::shared_lock lock(m_mutex);

    uint64_t acc = 0;
    auto     it  = seek(pos, &acc);
    if (it == m_pieces.cend()) return 0;

    size_t   written  = 0;
    uint64_t piecePos = pos - acc;
    while (it != m_pieces.cend() && written < len) {
        const uint64_t avail = it->length - piecePos;
        const size_t   want  = static_cast<size_t>(std::min<uint64_t>(avail, len - written));

        if (it->origin == PieceOrigin::File) {
            // slice() may come up short on the pread path; loop until the
            // piece's contribution is fully copied.
            size_t done = 0;
            while (done < want) {
                const std::string_view s =
                    m_file->slice(it->offset + piecePos + done, want - done);
                if (s.empty()) return written + done;
                std::memcpy(dst + written + done, s.data(), s.size());
                done += s.size();
            }
            written += done;
        } else {
            std::memcpy(dst + written, m_addBuffer.data() + it->offset + piecePos, want);
            written += want;
        }

        piecePos = 0;
        ++it;
    }
    return written;
}

std::string PieceTable::read(uint64_t pos, uint64_t len) const
{
    std::string out;
    if (len == 0) return out;
    out.resize(static_cast<size_t>(len));
    const size_t got = readInto(pos, out.data(), out.size());
    out.resize(got);
    return out;
}

void PieceTable::releasePagesBefore(uint64_t offset) const
{
    if (!m_file || offset == 0) return;
    std::shared_lock lock(m_mutex);

    // Resume where this thread's previous call stopped: callers invoke this
    // every few megabytes of a sequential pass, and re-walking [0, offset) each
    // time is quadratic in the piece count. Pages before `from` were already
    // advised away by the earlier call.
    struct Released {
        const PieceTable*                table = nullptr;
        uint64_t                         gen   = 0;
        std::list<Piece>::const_iterator it;
        uint64_t                         acc   = 0; // document offset of *it
    };
    static thread_local Released last;

    auto     it  = m_pieces.cbegin();
    uint64_t acc = 0;
    if (last.table == this && last.gen == m_generation && last.acc <= offset) {
        it  = last.it;
        acc = last.acc;
    }

    // Drop the file-backed pieces covering [acc, offset). ADD pieces are
    // ordinary heap memory and stay put.
    for (; it != m_pieces.cend() && acc < offset; ++it) {
        const uint64_t covered = std::min<uint64_t>(it->length, offset - acc);
        if (it->origin == PieceOrigin::File)
            m_file->adviseDontNeed(it->offset, covered);
        if (acc + it->length > offset) break; // partially covered: revisit it
        acc += it->length;
    }

    last = Released{this, m_generation, it, acc};
}

std::string_view PieceTable::chunkAt(uint64_t pos, size_t maxLen) const
{
    if (maxLen == 0) return {};
    std::shared_lock lock(m_mutex);

    uint64_t acc = 0;
    auto     it  = seek(pos, &acc);
    if (it == m_pieces.cend()) return {};

    const uint64_t piecePos = pos - acc;
    const size_t   want     = static_cast<size_t>(
        std::min<uint64_t>({it->length - piecePos, maxLen, kMaxChunk}));

    if (it->origin == PieceOrigin::File)
        return m_file->slice(it->offset + piecePos, want);
    return std::string_view(m_addBuffer).substr(
        static_cast<size_t>(it->offset + piecePos), want);
}

// --- Iterator ---

bool PieceTable::Iterator::atEnd() const
{
    return !m_table || m_it == m_table->m_pieces.cend();
}

std::string_view PieceTable::Iterator::nextChunk()
{
    if (atEnd()) return {};

    const Piece& p    = *m_it;
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(p.length - m_piecePos, kMaxChunk));

    std::string_view chunk;
    if (p.origin == PieceOrigin::File) {
        chunk = m_table->m_file->slice(p.offset + m_piecePos, want);
    } else {
        chunk = std::string_view(m_table->m_addBuffer)
                    .substr(static_cast<size_t>(p.offset + m_piecePos), want);
    }

    // A chunk can be short of the piece's remainder (kMaxChunk cap, or a short
    // pread); only advance to the next piece once this one is exhausted.
    m_piecePos += chunk.size();
    m_docPos   += chunk.size();
    if (chunk.empty() || m_piecePos >= p.length) {
        ++m_it;
        m_piecePos = 0;
    }
    return chunk;
}

PieceTable::Iterator PieceTable::begin() const
{
    Iterator it;
    it.m_table    = this;
    it.m_it       = m_pieces.cbegin();
    it.m_piecePos = 0;
    it.m_docPos   = 0;
    return it;
}

PieceTable::Iterator PieceTable::iteratorAt(uint64_t pos) const
{
    Iterator it;
    it.m_table    = this;
    it.m_piecePos = 0;
    it.m_docPos   = pos;

    uint64_t acc = 0;
    for (auto pit = m_pieces.cbegin(); pit != m_pieces.cend(); ++pit) {
        if (pos < acc + pit->length) {
            it.m_it       = pit;
            it.m_piecePos = pos - acc;
            return it;
        }
        acc += pit->length;
    }
    it.m_it = m_pieces.cend();
    return it;
}

// --- Private helpers ---

std::pair<std::list<Piece>::iterator, uint64_t> PieceTable::findPiece(uint64_t pos)
{
    uint64_t acc = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it) {
        // Strict <: a position landing exactly on a piece boundary belongs to
        // the *following* piece, so intra is never equal to the piece length.
        if (pos < acc + it->length)
            return {it, pos - acc};
        acc += it->length;
    }
    return {m_pieces.end(), 0};
}

std::list<Piece>::iterator PieceTable::splitAt(uint64_t pos)
{
    auto [it, intra] = findPiece(pos);
    if (it == m_pieces.end() || intra == 0) return it;

    const Piece tail{it->origin, it->offset + intra, it->length - intra};
    it->length = intra;
    return m_pieces.insert(std::next(it), tail);
}

void PieceTable::pushUndo(std::list<Piece> before, uint64_t cursorBefore, uint64_t cursorAfter)
{
    if (m_groupDepth > 0) {
        // Inside a group: the group's own pre-edit snapshot is the one that
        // matters, so only remember where the cursor started.
        if (!m_groupDirty) {
            m_groupCursorBefore = cursorBefore;
            m_groupDirty        = true;
        }
        return;
    }

    // Drop the redo tail.
    if (m_undoIndex + 1 < static_cast<int>(m_undoStack.size()))
        m_undoStack.erase(m_undoStack.begin() + m_undoIndex + 1, m_undoStack.end());

    m_undoStack.push_back({std::move(before), m_pieces, cursorBefore, cursorAfter});
    m_undoIndex = static_cast<int>(m_undoStack.size()) - 1;
    trimUndoToCap();
}

void PieceTable::trimUndoToCap()
{
    uint64_t total = 0;
    for (const auto& rec : m_undoStack)
        total += listBytes(rec.before) + listBytes(rec.after);
    if (total <= kUndoMemoryCap) return;

    // Discard oldest records until back under the cap, keeping at least the
    // most recent one so undo never becomes entirely unavailable.
    size_t drop = 0;
    while (drop + 1 < m_undoStack.size() && total > kUndoMemoryCap) {
        const auto& rec = m_undoStack[drop];
        total -= listBytes(rec.before) + listBytes(rec.after);
        ++drop;
    }
    if (drop == 0) return;

    m_undoStack.erase(m_undoStack.begin(), m_undoStack.begin() + static_cast<long>(drop));
    m_undoIndex     = std::max(-1, m_undoIndex - static_cast<int>(drop));
    m_undoTruncated = true;
}
