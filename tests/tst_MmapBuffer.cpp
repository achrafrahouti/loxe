#include <QtTest>
#include "engine/MmapBuffer.h"

class tst_MmapBuffer : public QObject {
    Q_OBJECT
private slots:
    void openNonexistent_returnsFalse();
    void openFile_isOpen();
    void size_matchesFileSize();
    void slice_returnsCorrectBytes();
    void sliceAtEnd_doesNotOverrun();
    void adviseSequentialAndRandom_doNotCrash();
    void moveSemantics_transfersOwnership();
};

void tst_MmapBuffer::openNonexistent_returnsFalse()
{
    MmapBuffer buf;
    QVERIFY(!buf.open("/nonexistent/path/to/file.xml"));
    QVERIFY(!buf.isOpen());
}

void tst_MmapBuffer::openFile_isOpen()           { QSKIP("TODO: create temp file"); }
void tst_MmapBuffer::size_matchesFileSize()       { QSKIP("TODO"); }
void tst_MmapBuffer::slice_returnsCorrectBytes()  { QSKIP("TODO"); }
void tst_MmapBuffer::sliceAtEnd_doesNotOverrun()  { QSKIP("TODO"); }
void tst_MmapBuffer::adviseSequentialAndRandom_doNotCrash() { QSKIP("TODO"); }
void tst_MmapBuffer::moveSemantics_transfersOwnership()     { QSKIP("TODO"); }

QTEST_MAIN(tst_MmapBuffer)
#include "tst_MmapBuffer.moc"
