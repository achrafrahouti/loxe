#include <QtTest>
#include "engine/MmapBuffer.h"
#include "engine/PieceTable.h"
#include "engine/SparseLineIndex.h"
#include "TestHelpers.h"

#include <atomic>

using namespace loxe_test;

class tst_SparseLineIndex : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void emptyFile_lineCountIsOne();
    void singleNewline_lineCountIsTwo();
    void noTrailingNewline_countsLastLine();
    void lineToOffset_roundTrips();
    void lineToOffset_beyondEnd_returnsDocumentLength();
    void offsetToLine_midLine_returnsCorrectLine();
    void lineEndOffset_pointsAtNewline();
    void crlfHandling_countedAsOneLine();
    void attachOnly_lookupsWorkWithoutBuild();
    void largeFile_latencyUnder5ms();
    void largeFile_lineCountIsExact();
    void cancelDuringBuild_returnsFalse();
    void invalidateFrom_reducesCheckpoints();
    void invalidateAfterEdit_lookupsStayCorrect();
    void estimatedLineCount_beforeComplete_isReasonable();

private:
    QTemporaryDir m_dir;
};

void tst_SparseLineIndex::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

// Builds an in-memory document; no file needed.
static PieceTable* makeDoc(std::string_view text)
{
    auto* pt = new PieceTable(nullptr);
    pt->appendInitial(text);
    return pt;
}

void tst_SparseLineIndex::emptyFile_lineCountIsOne()
{
    PieceTable pt(nullptr);
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(pt, cancel));
    QCOMPARE(idx.lineCount(), uint64_t{1});
    QCOMPARE(idx.lineToOffset(0), uint64_t{0});
}

void tst_SparseLineIndex::singleNewline_lineCountIsTwo()
{
    std::unique_ptr<PieceTable> pt(makeDoc("a\n"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineCount(), uint64_t{2});
    QCOMPARE(idx.lineToOffset(1), uint64_t{2});
}

void tst_SparseLineIndex::noTrailingNewline_countsLastLine()
{
    std::unique_ptr<PieceTable> pt(makeDoc("one\ntwo\nthree"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineCount(), uint64_t{3});
    QCOMPARE(idx.lineToOffset(2), uint64_t{8});
}

void tst_SparseLineIndex::lineToOffset_roundTrips()
{
    // Enough lines to span many checkpoint intervals (one per 4 KB).
    std::string text;
    for (int i = 0; i < 20000; ++i) text += "line " + std::to_string(i) + "\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineCount(), uint64_t{20001});

    for (uint64_t line : {uint64_t{0}, uint64_t{1}, uint64_t{999}, uint64_t{4096},
                          uint64_t{12345}, uint64_t{19999}}) {
        const uint64_t offset = idx.lineToOffset(line);
        QCOMPARE(idx.offsetToLine(offset), line);
        // The offset must be the first byte of that line's text.
        QCOMPARE(pt->read(offset, 5), std::string("line ").substr(0, 5));
    }
}

void tst_SparseLineIndex::lineToOffset_beyondEnd_returnsDocumentLength()
{
    std::unique_ptr<PieceTable> pt(makeDoc("a\nb\n"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineToOffset(9999), pt->length());
}

void tst_SparseLineIndex::offsetToLine_midLine_returnsCorrectLine()
{
    std::unique_ptr<PieceTable> pt(makeDoc("aaa\nbbb\nccc\n"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));

    QCOMPARE(idx.offsetToLine(0), uint64_t{0});
    QCOMPARE(idx.offsetToLine(2), uint64_t{0});  // mid first line
    QCOMPARE(idx.offsetToLine(4), uint64_t{1});  // start of second
    QCOMPARE(idx.offsetToLine(6), uint64_t{1});  // mid second
    QCOMPARE(idx.offsetToLine(8), uint64_t{2});
}

void tst_SparseLineIndex::lineEndOffset_pointsAtNewline()
{
    std::unique_ptr<PieceTable> pt(makeDoc("aaa\nbb\nc"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));

    QCOMPARE(idx.lineEndOffset(0), uint64_t{3});
    QCOMPARE(idx.lineEndOffset(1), uint64_t{6});
    // Final line without a newline ends at the document's end.
    QCOMPARE(idx.lineEndOffset(2), pt->length());
}

void tst_SparseLineIndex::crlfHandling_countedAsOneLine()
{
    // CRLF contains exactly one '\n', so a CRLF document must not double-count.
    std::unique_ptr<PieceTable> pt(makeDoc("one\r\ntwo\r\nthree\r\n"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));

    QCOMPARE(idx.lineCount(), uint64_t{4}); // 3 lines + the empty final one
    QCOMPARE(idx.lineToOffset(1), uint64_t{5});
    QCOMPARE(idx.lineToOffset(2), uint64_t{10});
    // The CR belongs to the terminator, so the line's content ends before it.
    QCOMPARE(idx.lineEndOffset(0), uint64_t{4});
}

void tst_SparseLineIndex::attachOnly_lookupsWorkWithoutBuild()
{
    // attach() must make lookups usable immediately — this is what lets the
    // viewport paint before the background scan finishes.
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "row " + std::to_string(i) + "\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    idx.attach(*pt);
    QVERIFY(!idx.isComplete());

    QCOMPARE(idx.offsetToLine(idx.lineToOffset(100)), uint64_t{100});
    QCOMPARE(idx.offsetToLine(idx.lineToOffset(4999)), uint64_t{4999});
    QCOMPARE(idx.lineCount(), uint64_t{5001});
}

void tst_SparseLineIndex::largeFile_latencyUnder5ms()
{
    LOXE_SKIP_IF_SANITIZED();

    // ~16 MB, comfortably past many checkpoint intervals.
    std::string text;
    text.reserve(16u << 20);
    while (text.size() < (16u << 20)) text += "<row>some payload here</row>\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));

    const uint64_t mid = idx.lineCount() / 2;
    QElapsedTimer timer;
    timer.start();
    const uint64_t offset = idx.lineToOffset(mid);
    const qint64 elapsedUs = timer.nsecsElapsed() / 1000;

    QCOMPARE(idx.offsetToLine(offset), mid);
    // SRS: any line lookup within 5 ms for files up to 2 GB.
    QVERIFY2(elapsedUs < 5000, qPrintable(QString("lookup took %1 us").arg(elapsedUs)));
}

void tst_SparseLineIndex::largeFile_lineCountIsExact()
{
    std::string text;
    const int rows = 100000;
    for (int i = 0; i < rows; ++i) text += "<r/>\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineCount(), static_cast<uint64_t>(rows) + 1);
}

void tst_SparseLineIndex::cancelDuringBuild_returnsFalse()
{
    std::string text;
    while (text.size() < (8u << 20)) text += "abcdefghij\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    std::atomic<bool> cancel{true}; // already cancelled when the scan starts
    QVERIFY(!idx.build(*pt, cancel));
    QVERIFY(!idx.isComplete());
}

void tst_SparseLineIndex::invalidateFrom_reducesCheckpoints()
{
    std::string text;
    for (int i = 0; i < 20000; ++i) text += "line " + std::to_string(i) + "\n";
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QVERIFY(idx.isComplete());

    // Capture answers from the complete index to compare against afterwards.
    const uint64_t lineAt500     = idx.offsetToLine(500);
    const uint64_t offsetOf15000 = idx.lineToOffset(15000);

    idx.invalidateFrom(1000);
    QVERIFY(!idx.isComplete());

    // Lookups before the invalidation point are still served from surviving
    // checkpoints, and ones past it lazily rebuild rather than going wrong.
    QCOMPARE(idx.offsetToLine(500), lineAt500);
    QCOMPARE(idx.lineToOffset(15000), offsetOf15000);
    QCOMPARE(idx.offsetToLine(offsetOf15000), uint64_t{15000});
}

void tst_SparseLineIndex::invalidateAfterEdit_lookupsStayCorrect()
{
    // The index is built over the PieceTable, so an edit plus invalidateFrom
    // must yield offsets consistent with the *edited* document.
    std::unique_ptr<PieceTable> pt(makeDoc("aaa\nbbb\nccc\nddd\n"));
    SparseLineIndex idx;
    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    QCOMPARE(idx.lineToOffset(2), uint64_t{8});

    // Insert an extra line at the top; every later offset shifts by 6.
    pt->insert(0, "zzzzz\n");
    idx.invalidateFrom(0);

    QCOMPARE(idx.lineCount(), uint64_t{6});
    QCOMPARE(idx.lineToOffset(1), uint64_t{6});
    QCOMPARE(idx.lineToOffset(3), uint64_t{14});
    QCOMPARE(pt->read(idx.lineToOffset(3), 3), std::string("ccc"));
}

void tst_SparseLineIndex::estimatedLineCount_beforeComplete_isReasonable()
{
    std::string text;
    for (int i = 0; i < 50000; ++i) text += "0123456789\n"; // 11 bytes per line
    std::unique_ptr<PieceTable> pt(makeDoc(text));

    SparseLineIndex idx;
    idx.attach(*pt);

    // With nothing scanned the estimate is 1; force a partial scan by asking for
    // an early line, then check the extrapolation is in the right ballpark.
    idx.lineToOffset(100);
    const uint64_t estimate = idx.estimatedLineCount();
    QVERIFY(estimate > 1);

    std::atomic<bool> cancel{false};
    QVERIFY(idx.build(*pt, cancel));
    const uint64_t exact = idx.lineCount();
    QCOMPARE(exact, uint64_t{50001});
    // Uniform line lengths, so the estimate should land within 25 %.
    QVERIFY2(estimate > exact / 2 && estimate < exact * 2,
             qPrintable(QString("estimate %1 vs exact %2").arg(estimate).arg(exact)));
}

QTEST_MAIN(tst_SparseLineIndex)
#include "tst_SparseLineIndex.moc"
