#include <QtTest>
#include "engine/PieceTable.h"
#include "engine/MmapBuffer.h"

class tst_PieceTable : public QObject {
    Q_OBJECT
private slots:
    void emptyTable_lengthIsZero();
    void insertAtStart_prependsText();
    void insertAtEnd_appendsText();
    void insertInMiddle_splicesPiece();
    void remove_deletesRange();
    void replace_deletesAndInserts();
    void undoInsert_restoresOriginal();
    void redoAfterUndo_reappliesEdit();
    void multipleUndoSteps_workInOrder();
    void iterator_readsAllBytes();
    void iterator_acrossMultiplePieces();
    void concurrentReads_doNotRace();
};

void tst_PieceTable::emptyTable_lengthIsZero()
{
    MmapBuffer buf; // not open → no FILE piece
    PieceTable pt(&buf);
    QCOMPARE(pt.length(), uint64_t{0});
}

void tst_PieceTable::insertAtStart_prependsText()
{
    MmapBuffer buf;
    PieceTable pt(&buf);
    pt.insert(0, "hello");
    QCOMPARE(pt.length(), uint64_t{5});
}

void tst_PieceTable::insertAtEnd_appendsText()       { QSKIP("TODO"); }
void tst_PieceTable::insertInMiddle_splicesPiece()   { QSKIP("TODO"); }
void tst_PieceTable::remove_deletesRange()           { QSKIP("TODO"); }
void tst_PieceTable::replace_deletesAndInserts()     { QSKIP("TODO"); }
void tst_PieceTable::undoInsert_restoresOriginal()   { QSKIP("TODO"); }
void tst_PieceTable::redoAfterUndo_reappliesEdit()   { QSKIP("TODO"); }
void tst_PieceTable::multipleUndoSteps_workInOrder() { QSKIP("TODO"); }
void tst_PieceTable::iterator_readsAllBytes()        { QSKIP("TODO"); }
void tst_PieceTable::iterator_acrossMultiplePieces() { QSKIP("TODO"); }
void tst_PieceTable::concurrentReads_doNotRace()     { QSKIP("TODO"); }

QTEST_MAIN(tst_PieceTable)
#include "tst_PieceTable.moc"
