#include <QtTest>
#include "engine/MmapBuffer.h"
#include "TestHelpers.h"

using namespace loxe_test;

class tst_MmapBuffer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();

    void openNonexistent_returnsFalse();
    void openDirectory_returnsFalse();
    void openFile_isOpen();
    void size_matchesFileSize();
    void emptyFile_opensWithZeroSize();
    void slice_returnsCorrectBytes();
    void sliceAtEnd_doesNotOverrun();
    void slicePastEnd_returnsEmpty();
    void sliceZeroLength_returnsEmpty();
    void adviseSequentialAndRandom_doNotCrash();
    void adviseDontNeed_keepsContentReadable();
    void moveConstruct_transfersOwnership();
    void moveAssign_transfersOwnership();
    void remap_picksUpNewContent();
    void close_resetsState();

private:
    QTemporaryDir m_dir;
    QString       m_path;
    QByteArray    m_content;
};

void tst_MmapBuffer::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_content = QByteArray("<?xml version=\"1.0\"?>\n<root><a>1</a></root>\n");
    m_path    = writeTempFile(m_dir, "sample.xml", m_content);
    QVERIFY(!m_path.isEmpty());
}

void tst_MmapBuffer::openNonexistent_returnsFalse()
{
    MmapBuffer buf;
    QVERIFY(!buf.open("/nonexistent/path/to/file.xml"));
    QVERIFY(!buf.isOpen());
    QCOMPARE(buf.size(), uint64_t{0});
}

void tst_MmapBuffer::openDirectory_returnsFalse()
{
    // mmap() of a directory can succeed on some kernels; the S_ISREG guard rejects it.
    MmapBuffer buf;
    QVERIFY(!buf.open(m_dir.path().toUtf8().constData()));
    QVERIFY(!buf.isOpen());
}

void tst_MmapBuffer::openFile_isOpen()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    QVERIFY(buf.isOpen());
    QCOMPARE(QString::fromStdString(buf.path()), m_path);
}

void tst_MmapBuffer::size_matchesFileSize()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    QCOMPARE(buf.size(), static_cast<uint64_t>(m_content.size()));
}

void tst_MmapBuffer::emptyFile_opensWithZeroSize()
{
    const QString path = writeTempFile(m_dir, "empty.xml", {});
    MmapBuffer buf;
    QVERIFY(buf.open(path.toUtf8().constData()));
    QCOMPARE(buf.size(), uint64_t{0});
    QVERIFY(buf.slice(0, 10).empty());
}

void tst_MmapBuffer::slice_returnsCorrectBytes()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));

    const std::string_view head = buf.slice(0, 5);
    QCOMPARE(QByteArray(head.data(), static_cast<int>(head.size())), QByteArray("<?xml"));

    const std::string_view mid = buf.slice(22, 6);
    QCOMPARE(QByteArray(mid.data(), static_cast<int>(mid.size())), QByteArray("<root>"));
}

void tst_MmapBuffer::sliceAtEnd_doesNotOverrun()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));

    // Asking for far more than remains must clamp, not read out of bounds.
    const std::string_view tail = buf.slice(buf.size() - 2, 4096);
    QCOMPARE(tail.size(), size_t{2});
}

void tst_MmapBuffer::slicePastEnd_returnsEmpty()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    QVERIFY(buf.slice(buf.size(), 1).empty());
    QVERIFY(buf.slice(buf.size() + 1000, 1).empty());
}

void tst_MmapBuffer::sliceZeroLength_returnsEmpty()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    QVERIFY(buf.slice(0, 0).empty());
}

void tst_MmapBuffer::adviseSequentialAndRandom_doNotCrash()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    buf.adviseSequential();
    buf.adviseRandom();
    QCOMPARE(buf.slice(0, 5).size(), size_t{5});
}

void tst_MmapBuffer::adviseDontNeed_keepsContentReadable()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    // MADV_DONTNEED drops the resident pages; the next read must fault them
    // back in rather than return zeroes.
    buf.adviseDontNeed(0, buf.size());
    const std::string_view head = buf.slice(0, 5);
    QCOMPARE(QByteArray(head.data(), static_cast<int>(head.size())), QByteArray("<?xml"));
}

void tst_MmapBuffer::moveConstruct_transfersOwnership()
{
    MmapBuffer src;
    QVERIFY(src.open(m_path.toUtf8().constData()));
    const uint64_t size = src.size();

    MmapBuffer dst(std::move(src));
    QVERIFY(dst.isOpen());
    QCOMPARE(dst.size(), size);
    QVERIFY(!src.isOpen());
    QCOMPARE(src.size(), uint64_t{0});
}

void tst_MmapBuffer::moveAssign_transfersOwnership()
{
    MmapBuffer src;
    QVERIFY(src.open(m_path.toUtf8().constData()));

    // The destination already owns a mapping, which must be released cleanly.
    MmapBuffer dst;
    QVERIFY(dst.open(m_path.toUtf8().constData()));

    dst = std::move(src);
    QVERIFY(dst.isOpen());
    QCOMPARE(dst.size(), static_cast<uint64_t>(m_content.size()));
    QVERIFY(!src.isOpen());
    QCOMPARE(dst.slice(0, 5).size(), size_t{5});
}

void tst_MmapBuffer::remap_picksUpNewContent()
{
    const QString path = writeTempFile(m_dir, "changing.xml", QByteArray("<a/>"));
    MmapBuffer buf;
    QVERIFY(buf.open(path.toUtf8().constData()));
    QCOMPARE(buf.size(), uint64_t{4});

    QVERIFY(!writeTempFile(m_dir, "changing.xml", QByteArray("<abc>xyz</abc>")).isEmpty());
    QVERIFY(buf.remap());
    QCOMPARE(buf.size(), uint64_t{14});

    const std::string_view all = buf.slice(0, buf.size());
    QCOMPARE(QByteArray(all.data(), static_cast<int>(all.size())), QByteArray("<abc>xyz</abc>"));
}

void tst_MmapBuffer::close_resetsState()
{
    MmapBuffer buf;
    QVERIFY(buf.open(m_path.toUtf8().constData()));
    buf.close();
    QVERIFY(!buf.isOpen());
    QCOMPARE(buf.size(), uint64_t{0});
    QVERIFY(buf.path().empty());
    QVERIFY(!buf.remap()); // nothing to remap once closed
}

QTEST_MAIN(tst_MmapBuffer)
#include "tst_MmapBuffer.moc"
