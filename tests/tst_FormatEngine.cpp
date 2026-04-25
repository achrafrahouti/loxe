#include <QtTest>
#include "engine/FormatEngine.h"
#include "engine/PieceTable.h"
#include "engine/MmapBuffer.h"

class tst_FormatEngine : public QObject {
    Q_OBJECT
private slots:
    void beautify_indentsElements();
    void beautify_preservesComments();
    void beautify_preservesCdata();
    void beautify_preservesProcessingInstructions();
    void beautify_preservesDeclaredEncoding();
    void minify_removesInsignificantWhitespace();
    void roundTrip_beautifyThenSave_byteIdentical();
    void cancellation_returnsNullptr();
    void progressCallback_calledDuringFormat();
    void throughput_atLeast50MBps();
    // Memory: output buffer never exceeds 8 MB during streaming
    void memoryUsage_outputBufferBounded();
};

void tst_FormatEngine::beautify_indentsElements()              { QSKIP("TODO: needs CMarkup"); }
void tst_FormatEngine::beautify_preservesComments()            { QSKIP("TODO"); }
void tst_FormatEngine::beautify_preservesCdata()               { QSKIP("TODO"); }
void tst_FormatEngine::beautify_preservesProcessingInstructions() { QSKIP("TODO"); }
void tst_FormatEngine::beautify_preservesDeclaredEncoding()    { QSKIP("TODO"); }
void tst_FormatEngine::minify_removesInsignificantWhitespace() { QSKIP("TODO"); }
void tst_FormatEngine::roundTrip_beautifyThenSave_byteIdentical() { QSKIP("TODO"); }
void tst_FormatEngine::cancellation_returnsNullptr()           { QSKIP("TODO"); }
void tst_FormatEngine::progressCallback_calledDuringFormat()   { QSKIP("TODO"); }
void tst_FormatEngine::throughput_atLeast50MBps()              { QSKIP("TODO"); }
void tst_FormatEngine::memoryUsage_outputBufferBounded()       { QSKIP("TODO"); }

QTEST_MAIN(tst_FormatEngine)
#include "tst_FormatEngine.moc"
