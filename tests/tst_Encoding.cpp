#include <QtTest>
#include "engine/Encoding.h"
#include "engine/PieceTable.h"
#include "TestHelpers.h"

#include <memory>

class tst_Encoding : public QObject {
    Q_OBJECT
private slots:
    void utf8Bom_detected();
    void utf16LeBom_detected();
    void utf16BeBom_detected();
    void xmlDeclaration_encodingHonoured();
    void xmlDeclaration_singleQuoted();
    void xmlDeclaration_withSpacesAroundEquals();
    void noDeclaration_validUtf8_defaultsToUtf8();
    void noDeclaration_invalidUtf8_reportsLatin1();
    void emptyDocument_defaultsToUtf8();
    void bomTakesPrecedenceOverDeclaration();

    void isValidUtf8_acceptsMultiByte();
    void isValidUtf8_rejectsBadContinuation();
    void isValidUtf8_toleratesTruncatedTail();

private:
    static std::unique_ptr<PieceTable> doc(const QByteArray& bytes)
    {
        auto pt = std::make_unique<PieceTable>(nullptr);
        pt->appendInitial(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
        return pt;
    }
};

void tst_Encoding::utf8Bom_detected()
{
    auto pt = doc(QByteArray("\xEF\xBB\xBF<a/>"));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("UTF-8"));
    QVERIFY(info.hasBom);
    QCOMPARE(info.bomBytes, size_t{3});
}

void tst_Encoding::utf16LeBom_detected()
{
    auto pt = doc(QByteArray("\xFF\xFE<\0a\0", 6));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("UTF-16LE"));
    QVERIFY(info.hasBom);
    QCOMPARE(info.bomBytes, size_t{2});
}

void tst_Encoding::utf16BeBom_detected()
{
    auto pt = doc(QByteArray("\xFE\xFF\0<\0a", 6));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("UTF-16BE"));
    QVERIFY(info.hasBom);
}

void tst_Encoding::xmlDeclaration_encodingHonoured()
{
    auto pt = doc(QByteArray("<?xml version=\"1.0\" encoding=\"iso-8859-1\"?><a/>"));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("ISO-8859-1"));
    QVERIFY(!info.hasBom);
}

void tst_Encoding::xmlDeclaration_singleQuoted()
{
    auto pt = doc(QByteArray("<?xml version='1.0' encoding='windows-1252'?><a/>"));
    QCOMPARE(QString::fromStdString(Encoding::detect(*pt).name), QStringLiteral("WINDOWS-1252"));
}

void tst_Encoding::xmlDeclaration_withSpacesAroundEquals()
{
    auto pt = doc(QByteArray("<?xml version=\"1.0\" encoding = \"UTF-16\"?><a/>"));
    QCOMPARE(QString::fromStdString(Encoding::detect(*pt).name), QStringLiteral("UTF-16"));
}

void tst_Encoding::noDeclaration_validUtf8_defaultsToUtf8()
{
    auto pt = doc(QByteArray("<a>caf\xC3\xA9</a>"));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("UTF-8"));
    QVERIFY(!info.hasBom);
}

void tst_Encoding::noDeclaration_invalidUtf8_reportsLatin1()
{
    // 0xE9 alone is 'é' in Latin-1 but an invalid UTF-8 lead sequence.
    auto pt = doc(QByteArray("<a>caf\xE9 x</a>"));
    QCOMPARE(QString::fromStdString(Encoding::detect(*pt).name), QStringLiteral("ISO-8859-1"));
}

void tst_Encoding::emptyDocument_defaultsToUtf8()
{
    auto pt = doc({});
    QCOMPARE(QString::fromStdString(Encoding::detect(*pt).name), QStringLiteral("UTF-8"));
}

void tst_Encoding::bomTakesPrecedenceOverDeclaration()
{
    // ENC-01 order: BOM wins even when the declaration disagrees.
    auto pt = doc(QByteArray("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>"));
    const auto info = Encoding::detect(*pt);
    QCOMPARE(QString::fromStdString(info.name), QStringLiteral("UTF-8"));
    QVERIFY(info.hasBom);
}

void tst_Encoding::isValidUtf8_acceptsMultiByte()
{
    QVERIFY(Encoding::isValidUtf8("plain ascii"));
    QVERIFY(Encoding::isValidUtf8("caf\xC3\xA9"));           // 2-byte
    QVERIFY(Encoding::isValidUtf8("\xE2\x82\xAC"));          // 3-byte €
    QVERIFY(Encoding::isValidUtf8("\xF0\x9F\x98\x80"));      // 4-byte emoji
}

void tst_Encoding::isValidUtf8_rejectsBadContinuation()
{
    // Split the literals: "\x80abc" would parse as one oversized hex escape
    // because a, b and c are themselves hex digits.
    QVERIFY(!Encoding::isValidUtf8("\xC3" "zz"));  // continuation byte missing
    QVERIFY(!Encoding::isValidUtf8("\x80" "abc")); // stray continuation byte
    QVERIFY(!Encoding::isValidUtf8("\xFF\xFE"));   // invalid lead bytes
}

void tst_Encoding::isValidUtf8_toleratesTruncatedTail()
{
    // The probe cuts the document at a fixed size, so a sequence split by the
    // probe boundary must not be reported as invalid.
    QVERIFY(Encoding::isValidUtf8(std::string_view("abc\xE2\x82", 5)));
}

QTEST_MAIN(tst_Encoding)
#include "tst_Encoding.moc"
