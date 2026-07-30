#include <QtTest>
#include "engine/PieceTable.h"
#include "engine/Validator.h"
#include "TestHelpers.h"

#include <atomic>
#include <memory>

using namespace loxe_test;

class tst_Validator : public QObject {
    Q_OBJECT
private slots:
    void wellFormed_simpleDocument();
    void wellFormed_withDeclarationCommentsAndCdata();
    void wellFormed_withNamespaces();
    void wellFormed_selfClosingRoot();
    void emptyDocument_treatedAsClean();

    void mismatchedTag_reportsError();
    void unclosedTag_reportsError();
    void strayLessThan_reportsError();
    void twoRootElements_reportsError();
    void undefinedEntity_reportsError();

    void diagnostic_carriesLineNumber();
    void diagnostics_areCapped();
    void fatalError_stopsAtFirstDiagnostic();

    void cancellation_stopsParse();
    void largeDocument_isNotSizeCapped();
    void hugeTextNode_doesNotTripLibxmlLimit();
    void externalDtd_isNotFetched();

private:
    static std::unique_ptr<PieceTable> doc(std::string_view text)
    {
        auto pt = std::make_unique<PieceTable>(nullptr);
        pt->appendInitial(text);
        return pt;
    }
};

// --- Accepting valid documents ---

void tst_Validator::wellFormed_simpleDocument()
{
    auto pt = doc("<root><a id=\"1\">text</a><b/></root>");
    const auto r = Validator::validate(*pt);
    QVERIFY2(r.wellFormed, r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
    QVERIFY(r.diagnostics.empty());
    QVERIFY(!r.cancelled);
}

void tst_Validator::wellFormed_withDeclarationCommentsAndCdata()
{
    auto pt = doc("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<!-- a comment with <angle> brackets -->\n"
                  "<root><![CDATA[ raw <stuff> & things ]]></root>\n");
    const auto r = Validator::validate(*pt);
    QVERIFY2(r.wellFormed, r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
}

void tst_Validator::wellFormed_withNamespaces()
{
    auto pt = doc("<x:root xmlns:x=\"urn:example\"><x:child/></x:root>");
    const auto r = Validator::validate(*pt);
    QVERIFY2(r.wellFormed, r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
}

void tst_Validator::wellFormed_selfClosingRoot()
{
    auto pt = doc("<root/>");
    QVERIFY(Validator::validate(*pt).wellFormed);
}

void tst_Validator::emptyDocument_treatedAsClean()
{
    // Strictly not well-formed (no root element), but flagging a blank editor
    // as broken is unhelpful noise.
    auto pt = doc("");
    const auto r = Validator::validate(*pt);
    QVERIFY(r.wellFormed);
    QVERIFY(r.diagnostics.empty());
}

// --- Rejecting invalid documents ---

void tst_Validator::mismatchedTag_reportsError()
{
    auto pt = doc("<root><a></b></root>");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
    QVERIFY(!r.diagnostics.empty());
    QVERIFY(r.primary() != nullptr);
}

void tst_Validator::unclosedTag_reportsError()
{
    auto pt = doc("<root><a>text</root>");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
    QVERIFY(!r.diagnostics.empty());
}

void tst_Validator::strayLessThan_reportsError()
{
    auto pt = doc("<root>a < b</root>");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
}

void tst_Validator::twoRootElements_reportsError()
{
    auto pt = doc("<a/><b/>");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
}

void tst_Validator::undefinedEntity_reportsError()
{
    auto pt = doc("<root>&nope;</root>");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
}

// --- Diagnostics ---

void tst_Validator::diagnostic_carriesLineNumber()
{
    // The mismatch is on line 4; the parser must say so, since the status bar
    // and the (future) errors panel navigate by it.
    auto pt = doc("<root>\n"
                  "  <a/>\n"
                  "  <b>\n"
                  "  </c>\n"
                  "</root>\n");
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
    const XmlDiagnostic* first = r.primary();
    QVERIFY(first != nullptr);
    QCOMPARE(first->line, 4);
    QVERIFY(!first->message.empty());
    // Messages are stored without their trailing newline.
    QVERIFY(first->message.back() != '\n');
}

void tst_Validator::diagnostics_areCapped()
{
    // Undefined namespace prefixes are reported as non-fatal errors, so the
    // parser keeps going and produces one per element — which is exactly the
    // runaway case the cap exists for.
    std::string text = "<root>\n";
    for (int i = 0; i < 500; ++i) text += "  <undef:x" + std::to_string(i) + "/>\n";
    text += "</root>\n";

    auto pt = doc(text);
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
    QCOMPARE(r.diagnostics.size(), Validator::kMaxDiagnostics);
    QVERIFY(r.truncated);
}

void tst_Validator::fatalError_stopsAtFirstDiagnostic()
{
    // A fatal error aborts the parse, so only one complaint comes back however
    // much broken markup follows. Worth pinning: it is why the errors panel
    // cannot promise a complete list for badly broken documents.
    std::string text = "<root>\n";
    for (int i = 0; i < 500; ++i) text += "  & bad\n";
    text += "</root>\n";

    auto pt = doc(text);
    const auto r = Validator::validate(*pt);
    QVERIFY(!r.wellFormed);
    QCOMPARE(r.diagnostics.size(), size_t{1});
    QVERIFY(!r.truncated);
    QVERIFY(r.primary()->fatal);
}

// --- Streaming behaviour ---

void tst_Validator::cancellation_stopsParse()
{
    std::string text = "<root>";
    for (int i = 0; i < 200000; ++i) text += "<item>value</item>";
    text += "</root>";

    auto pt = doc(text);
    std::atomic<bool> cancelled{true}; // already set before the first read
    const auto r = Validator::validate(*pt, &cancelled);
    QVERIFY(r.cancelled);
    QVERIFY(!r.wellFormed);
}

void tst_Validator::largeDocument_isNotSizeCapped()
{
    // The whole point of moving to libxml2: documents past the old 256 MB
    // in-memory ceiling are still checked. 40 MB keeps the test quick while
    // exercising many streaming pulls.
    std::string text = "<catalogue>\n";
    while (text.size() < (40u << 20))
        text += "  <book id=\"b\"><title>A Title</title><year>2024</year></book>\n";
    text += "</catalogue>\n";

    auto pt = doc(text);
    QVERIFY(pt->length() > (40u << 20));

    const auto r = Validator::validate(*pt);
    QVERIFY2(r.wellFormed, r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
}

void tst_Validator::hugeTextNode_doesNotTripLibxmlLimit()
{
    // libxml2 caps text nodes at 10 MB unless XML_PARSE_HUGE is set. A
    // large-file editor must not report a legitimate document as broken.
    std::string text = "<root>";
    text += std::string(12u << 20, 'x');
    text += "</root>";

    auto pt = doc(text);
    const auto r = Validator::validate(*pt);
    QVERIFY2(r.wellFormed, r.diagnostics.empty() ? "" : r.diagnostics[0].message.c_str());
}

void tst_Validator::externalDtd_isNotFetched()
{
    // XML_PARSE_NONET and no DTDLOAD: a remote DTD reference must neither be
    // fetched nor make the document unparseable.
    auto pt = doc("<?xml version=\"1.0\"?>\n"
                  "<!DOCTYPE root SYSTEM \"http://example.invalid/does-not-exist.dtd\">\n"
                  "<root><a/></root>\n");
    const auto r = Validator::validate(*pt);
    QVERIFY(r.wellFormed);
}

QTEST_MAIN(tst_Validator)
#include "tst_Validator.moc"
