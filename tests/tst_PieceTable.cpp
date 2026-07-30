#include <QtTest>
#include "engine/MmapBuffer.h"
#include "engine/PieceTable.h"
#include "TestHelpers.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace loxe_test;

class tst_PieceTable : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void emptyTable_lengthIsZero();
    void fileBacked_lengthIsFileSize();
    void insertAtStart_prependsText();
    void insertAtEnd_appendsText();
    void insertInMiddle_splicesPiece();
    void insertBeyondEnd_clampsToEnd();
    void consecutiveAppends_coalescePieces();

    void remove_deletesRange();
    void remove_atStart();
    void remove_toEnd();
    void remove_spanningMultiplePieces();
    void remove_pastEnd_clamps();
    void remove_everything_leavesEmpty();

    void replace_deletesAndInserts();
    void replaceAll_swapsWholeDocument();

    void undoInsert_restoresOriginal();
    void undoRemove_restoresOriginal();
    void redoAfterUndo_reappliesEdit();
    void multipleUndoSteps_workInOrder();
    void undoGroup_collapsesToSingleStep();
    void editAfterUndo_dropsRedoTail();
    void clearUndo_disablesUndo();

    void readInto_shortAtEndOfDocument();
    void read_acrossPieceBoundary();
    void chunkAt_isBounded();

    void iterator_readsAllBytes();
    void iterator_acrossMultiplePieces();
    void iterator_atOffset();
    void concurrentReads_doNotRace();

private:
    QTemporaryDir m_dir;
    QString       m_path;
    QByteArray    m_content;
};

void tst_PieceTable::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_content = QByteArray("0123456789");
    m_path    = writeTempFile(m_dir, "digits.txt", m_content);
    QVERIFY(!m_path.isEmpty());
}

// --- Construction ---

void tst_PieceTable::emptyTable_lengthIsZero()
{
    MmapBuffer buf; // not open → no FILE piece
    PieceTable pt(&buf);
    QCOMPARE(pt.length(), uint64_t{0});
    QVERIFY(dump(pt).empty());
}

void tst_PieceTable::fileBacked_lengthIsFileSize()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);
    QCOMPARE(pt.length(), uint64_t{10});
    QCOMPARE(dump(pt), std::string("0123456789"));
}

// --- Insert ---

void tst_PieceTable::insertAtStart_prependsText()
{
    MmapBuffer buf;
    PieceTable pt(&buf);
    pt.insert(0, "hello");
    QCOMPARE(pt.length(), uint64_t{5});
    QCOMPARE(dump(pt), std::string("hello"));

    pt.insert(0, ">>");
    QCOMPARE(dump(pt), std::string(">>hello"));
}

void tst_PieceTable::insertAtEnd_appendsText()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.insert(pt.length(), "ABC");
    QCOMPARE(pt.length(), uint64_t{13});
    QCOMPARE(dump(pt), std::string("0123456789ABC"));
}

void tst_PieceTable::insertInMiddle_splicesPiece()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.insert(5, "-X-");
    QCOMPARE(dump(pt), std::string("01234-X-56789"));
    QCOMPARE(pt.length(), uint64_t{13});
}

void tst_PieceTable::insertBeyondEnd_clampsToEnd()
{
    MmapBuffer buf;
    PieceTable pt(&buf);
    pt.insert(0, "abc");
    pt.insert(9999, "Z");
    QCOMPARE(dump(pt), std::string("abcZ"));
}

void tst_PieceTable::consecutiveAppends_coalescePieces()
{
    MmapBuffer buf;
    PieceTable pt(&buf);
    // Typing one character at a time must not add one piece per keystroke.
    for (int i = 0; i < 100; ++i) pt.insert(pt.length(), "x");
    QCOMPARE(pt.length(), uint64_t{100});
    QCOMPARE(pt.pieceCount(), size_t{1});
}

// --- Remove ---

void tst_PieceTable::remove_deletesRange()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(3, 4); // drop "3456"
    QCOMPARE(dump(pt), std::string("012789"));
    QCOMPARE(pt.length(), uint64_t{6});
}

void tst_PieceTable::remove_atStart()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(0, 3);
    QCOMPARE(dump(pt), std::string("3456789"));
}

void tst_PieceTable::remove_toEnd()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(7, 3);
    QCOMPARE(dump(pt), std::string("0123456"));
}

void tst_PieceTable::remove_spanningMultiplePieces()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    // Build several pieces, then delete across all of them.
    pt.insert(2, "AA");   // 01AA23456789
    pt.insert(7, "BB");   // 01AA234BB56789
    QCOMPARE(dump(pt), std::string("01AA234BB56789"));
    QVERIFY(pt.pieceCount() >= 4);

    pt.remove(1, 10);     // drop "1AA234BB56"
    QCOMPARE(dump(pt), std::string("0789"));
}

void tst_PieceTable::remove_pastEnd_clamps()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(8, 9999);
    QCOMPARE(dump(pt), std::string("01234567"));

    // Removing entirely past the end is a no-op.
    const std::string before = dump(pt);
    pt.remove(500, 10);
    QCOMPARE(dump(pt), before);
}

void tst_PieceTable::remove_everything_leavesEmpty()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(0, pt.length());
    QCOMPARE(pt.length(), uint64_t{0});
    QVERIFY(dump(pt).empty());
}

// --- Replace ---

void tst_PieceTable::replace_deletesAndInserts()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.replace(2, 3, "xyz!");
    QCOMPARE(dump(pt), std::string("01xyz!56789"));
}

void tst_PieceTable::replaceAll_swapsWholeDocument()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.replaceAll("<new/>");
    QCOMPARE(dump(pt), std::string("<new/>"));

    // One undo step must restore the original.
    pt.undo();
    QCOMPARE(dump(pt), std::string("0123456789"));
}

// --- Undo / redo ---

void tst_PieceTable::undoInsert_restoresOriginal()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    QVERIFY(!pt.canUndo());
    pt.insert(5, "INS");
    QCOMPARE(dump(pt), std::string("01234INS56789"));
    QVERIFY(pt.canUndo());

    uint64_t cursor = 0;
    pt.undo(&cursor);
    QCOMPARE(dump(pt), std::string("0123456789"));
    QCOMPARE(cursor, uint64_t{5});
    QVERIFY(!pt.canUndo());
}

void tst_PieceTable::undoRemove_restoresOriginal()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.remove(2, 5);
    QCOMPARE(dump(pt), std::string("01789"));
    pt.undo();
    QCOMPARE(dump(pt), std::string("0123456789"));
}

void tst_PieceTable::redoAfterUndo_reappliesEdit()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.insert(0, "Z");
    pt.undo();
    QVERIFY(pt.canRedo());
    pt.redo();
    QCOMPARE(dump(pt), std::string("Z0123456789"));
    QVERIFY(!pt.canRedo());
}

void tst_PieceTable::multipleUndoSteps_workInOrder()
{
    MmapBuffer buf;
    PieceTable pt(&buf);

    pt.insert(0, "one");
    pt.insert(3, "two");
    pt.insert(6, "three");
    QCOMPARE(dump(pt), std::string("onetwothree"));

    pt.undo();
    QCOMPARE(dump(pt), std::string("onetwo"));
    pt.undo();
    QCOMPARE(dump(pt), std::string("one"));
    pt.undo();
    QCOMPARE(dump(pt), std::string(""));
    QVERIFY(!pt.canUndo());

    pt.redo();
    QCOMPARE(dump(pt), std::string("one"));
    pt.redo();
    QCOMPARE(dump(pt), std::string("onetwo"));
}

void tst_PieceTable::undoGroup_collapsesToSingleStep()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.beginUndoGroup();
    pt.remove(0, 2);
    pt.insert(0, "AA");
    pt.insert(4, "BB");
    pt.endUndoGroup();
    QCOMPARE(dump(pt), std::string("AA23BB456789"));

    // The whole group is one step (SRS: beautify / replace-all are single undos).
    pt.undo();
    QCOMPARE(dump(pt), std::string("0123456789"));
    QVERIFY(!pt.canUndo());

    pt.redo();
    QCOMPARE(dump(pt), std::string("AA23BB456789"));
}

void tst_PieceTable::editAfterUndo_dropsRedoTail()
{
    MmapBuffer buf;
    PieceTable pt(&buf);

    pt.insert(0, "a");
    pt.insert(1, "b");
    pt.undo();
    QCOMPARE(dump(pt), std::string("a"));
    QVERIFY(pt.canRedo());

    pt.insert(1, "c");
    QCOMPARE(dump(pt), std::string("ac"));
    QVERIFY(!pt.canRedo()); // "b" is unreachable now
}

void tst_PieceTable::clearUndo_disablesUndo()
{
    MmapBuffer buf;
    PieceTable pt(&buf);
    pt.insert(0, "abc");
    QVERIFY(pt.canUndo());
    pt.clearUndo();
    QVERIFY(!pt.canUndo());
    QVERIFY(!pt.canRedo());
    QCOMPARE(dump(pt), std::string("abc"));
}

// --- Reading ---

void tst_PieceTable::readInto_shortAtEndOfDocument()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    char dst[64] = {};
    QCOMPARE(pt.readInto(8, dst, sizeof(dst)), size_t{2});
    QCOMPARE(std::string(dst, 2), std::string("89"));

    QCOMPARE(pt.readInto(10, dst, sizeof(dst)), size_t{0});
    QCOMPARE(pt.readInto(999, dst, sizeof(dst)), size_t{0});
}

void tst_PieceTable::read_acrossPieceBoundary()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.insert(5, "|MID|");
    // Spans FILE, ADD, then FILE again.
    QCOMPARE(pt.read(3, 9), std::string("34|MID|56"));
}

void tst_PieceTable::chunkAt_isBounded()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    const std::string_view c = pt.chunkAt(0, 4);
    QCOMPARE(c.size(), size_t{4});
    QCOMPARE(std::string(c), std::string("0123"));
    QVERIFY(pt.chunkAt(pt.length(), 10).empty());
}

// --- Iterator ---

void tst_PieceTable::iterator_readsAllBytes()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);
    QCOMPARE(dumpViaIterator(pt), std::string("0123456789"));
}

void tst_PieceTable::iterator_acrossMultiplePieces()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    pt.insert(4, "AAA");
    pt.insert(0, "<");
    pt.remove(6, 2);
    QVERIFY(pt.pieceCount() >= 3);
    // The iterator and the random-access reader must agree.
    QCOMPARE(dumpViaIterator(pt), dump(pt));
}

void tst_PieceTable::iterator_atOffset()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);

    auto it = pt.iteratorAt(6);
    std::string out;
    while (!it.atEnd()) {
        const std::string_view chunk = it.nextChunk();
        out.append(chunk.data(), chunk.size());
    }
    QCOMPARE(out, std::string("6789"));

    QVERIFY(pt.iteratorAt(pt.length()).atEnd());
}

void tst_PieceTable::concurrentReads_doNotRace()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    PieceTable pt(&buf);
    pt.insert(5, "|MID|");
    const std::string expected = dump(pt);

    // Several reader threads while the shared_mutex is held in shared mode.
    // Under the Debug build this also runs against ASan/TSan-style checks.
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&pt, &expected, &failures] {
            for (int i = 0; i < 200; ++i) {
                if (pt.read(0, pt.length()) != expected) ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();
    QCOMPARE(failures.load(), 0);
}

QTEST_MAIN(tst_PieceTable)
#include "tst_PieceTable.moc"
