#include <QtTest>
#include "engine/SparseLineIndex.h"
#include "engine/MmapBuffer.h"

class tst_SparseLineIndex : public QObject {
    Q_OBJECT
private slots:
    void emptyFile_lineCountIsOne();
    void singleNewline_lineCountIsTwo();
    void lineToOffset_roundTrips();
    void offsetToLine_midLine_returnsCorrectLine();
    void crlfHandling_countedAsOneLine();
    void largeFile_latencyUnder5ms();
    void cancelDuringBuild_returnsFalse();
    void invalidateFrom_reducesCheckpoints();
    void estimatedLineCount_beforeComplete_isReasonable();
};

void tst_SparseLineIndex::emptyFile_lineCountIsOne()   { QSKIP("TODO: needs temp file"); }
void tst_SparseLineIndex::singleNewline_lineCountIsTwo(){ QSKIP("TODO"); }

void tst_SparseLineIndex::lineToOffset_roundTrips()
{
    // Verifies lineToOffset(offsetToLine(x)) == x for checkpoint-aligned offsets
    QSKIP("TODO");
}

void tst_SparseLineIndex::offsetToLine_midLine_returnsCorrectLine() { QSKIP("TODO"); }
void tst_SparseLineIndex::crlfHandling_countedAsOneLine()           { QSKIP("TODO"); }
void tst_SparseLineIndex::largeFile_latencyUnder5ms()               { QSKIP("TODO"); }
void tst_SparseLineIndex::cancelDuringBuild_returnsFalse()          { QSKIP("TODO"); }
void tst_SparseLineIndex::invalidateFrom_reducesCheckpoints()       { QSKIP("TODO"); }
void tst_SparseLineIndex::estimatedLineCount_beforeComplete_isReasonable() { QSKIP("TODO"); }

QTEST_MAIN(tst_SparseLineIndex)
#include "tst_SparseLineIndex.moc"
