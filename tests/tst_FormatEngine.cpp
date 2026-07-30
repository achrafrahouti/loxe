#include <QtTest>
#include "engine/FormatEngine.h"
#include "engine/MmapBuffer.h"
#include "engine/PieceTable.h"
#include "TestHelpers.h"

#include <memory>

using namespace loxe_test;

class tst_FormatEngine : public QObject {
    Q_OBJECT
private slots:
    void beautify_indentsElements();
    void beautify_keepsTextOnlyElementInline();
    void beautify_preservesComments();
    void beautify_preservesCdata();
    void beautify_preservesProcessingInstructions();
    void beautify_preservesDoctypeWithInternalSubset();
    void beautify_preservesDeclaredEncoding();
    void beautify_preservesAttributeQuotingAndAngleBrackets();
    void beautify_tabIndentStyle();
    void beautify_customIndentWidth();
    void beautify_isIdempotent();
    void minify_removesInsignificantWhitespace();
    void minify_keepsSignificantText();
    void roundTrip_beautifyThenMinify_preservesContent();
    void roundTrip_beautifySaveReopen_isByteIdentical();
    void cancellation_returnsNullptr();
    void progressCallback_calledDuringFormat();
    void sinkAbort_returnsFalse();
    void emptyDocument_producesNothing();
    void throughput_atLeast50MBps();
    void memoryUsage_outputBufferBounded();

private:
    static std::unique_ptr<PieceTable> doc(std::string_view text)
    {
        auto pt = std::make_unique<PieceTable>(nullptr);
        pt->appendInitial(text);
        return pt;
    }

    static std::string beautify(std::string_view input,
                                FormatEngine::IndentStyle style = FormatEngine::IndentStyle::Spaces,
                                int width = 2)
    {
        auto src = doc(input);
        FormatEngine engine;
        FormatEngine::Options opts;
        opts.mode        = FormatEngine::Mode::Beautify;
        opts.indentStyle = style;
        opts.indentWidth = width;
        auto out = engine.format(*src, opts);
        return out ? dump(*out) : std::string();
    }

    static std::string minify(std::string_view input)
    {
        auto src = doc(input);
        FormatEngine engine;
        FormatEngine::Options opts;
        opts.mode = FormatEngine::Mode::Minify;
        auto out = engine.format(*src, opts);
        return out ? dump(*out) : std::string();
    }
};

void tst_FormatEngine::beautify_indentsElements()
{
    const std::string out = beautify("<a><b><c/></b></a>");
    QCOMPARE(out, std::string("<a>\n  <b>\n    <c/>\n  </b>\n</a>\n"));
}

void tst_FormatEngine::beautify_keepsTextOnlyElementInline()
{
    // An element whose only content is text stays on one line.
    const std::string out = beautify("<r><name>value</name></r>");
    QCOMPARE(out, std::string("<r>\n  <name>value</name>\n</r>\n"));
}

void tst_FormatEngine::beautify_preservesComments()
{
    const std::string out = beautify("<a><!-- keep <this> intact --><b/></a>");
    QVERIFY2(out.find("<!-- keep <this> intact -->") != std::string::npos, out.c_str());
    QCOMPARE(out, std::string("<a>\n  <!-- keep <this> intact -->\n  <b/>\n</a>\n"));
}

void tst_FormatEngine::beautify_preservesCdata()
{
    // CDATA content is opaque: angle brackets and whitespace must survive.
    const std::string out = beautify("<a><![CDATA[ if (x<y) { }  ]]></a>");
    QVERIFY2(out.find("<![CDATA[ if (x<y) { }  ]]>") != std::string::npos, out.c_str());
}

void tst_FormatEngine::beautify_preservesProcessingInstructions()
{
    const std::string out = beautify("<?xml-stylesheet href=\"a.xsl\"?><a/>");
    QVERIFY2(out.find("<?xml-stylesheet href=\"a.xsl\"?>") != std::string::npos, out.c_str());
}

void tst_FormatEngine::beautify_preservesDoctypeWithInternalSubset()
{
    // The '>' inside the internal subset must not terminate the DOCTYPE early.
    const std::string in  = "<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]><r/>";
    const std::string out = beautify(in);
    QVERIFY2(out.find("<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]>") != std::string::npos, out.c_str());
    QVERIFY2(out.find("<r/>") != std::string::npos, out.c_str());
}

void tst_FormatEngine::beautify_preservesDeclaredEncoding()
{
    const std::string out = beautify("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>");
    QVERIFY2(out.find("encoding=\"ISO-8859-1\"") != std::string::npos, out.c_str());
}

void tst_FormatEngine::beautify_preservesAttributeQuotingAndAngleBrackets()
{
    // A '>' inside an attribute value must not be read as the end of the tag.
    const std::string out = beautify("<a t=\"x > y\" u='v'><b/></a>");
    QVERIFY2(out.find("<a t=\"x > y\" u='v'>") != std::string::npos, out.c_str());
    QVERIFY2(out.find("<b/>") != std::string::npos, out.c_str());
}

void tst_FormatEngine::beautify_tabIndentStyle()
{
    const std::string out = beautify("<a><b/></a>", FormatEngine::IndentStyle::Tabs);
    QCOMPARE(out, std::string("<a>\n\t<b/>\n</a>\n"));
}

void tst_FormatEngine::beautify_customIndentWidth()
{
    const std::string out = beautify("<a><b/></a>", FormatEngine::IndentStyle::Spaces, 4);
    QCOMPARE(out, std::string("<a>\n    <b/>\n</a>\n"));
}

void tst_FormatEngine::beautify_isIdempotent()
{
    // Beautifying already-beautified output must be a no-op, or repeated
    // Ctrl+Shift+B would keep growing the document.
    const std::string once  = beautify("<a><b>t</b><c><d/></c></a>");
    const std::string twice = beautify(once);
    QCOMPARE(twice, once);
}

void tst_FormatEngine::minify_removesInsignificantWhitespace()
{
    const std::string out = minify("<a>\n  <b>\n    <c/>\n  </b>\n</a>\n");
    QCOMPARE(out, std::string("<a><b><c/></b></a>"));
}

void tst_FormatEngine::minify_keepsSignificantText()
{
    const std::string out = minify("<a>\n  <b>hello world</b>\n</a>\n");
    QCOMPARE(out, std::string("<a><b>hello world</b></a>"));
}

void tst_FormatEngine::roundTrip_beautifyThenMinify_preservesContent()
{
    const std::string original = "<r><a x=\"1\">t</a><b><c/></b><!--k--></r>";
    const std::string result   = minify(beautify(original));
    QCOMPARE(result, original);
}

// SRS: open → beautify → save → open must be byte-identical. This also covers
// the save path, which streams the piece list through the iterator exactly as
// MainWindow::writeDocumentTo() does.
void tst_FormatEngine::roundTrip_beautifySaveReopen_isByteIdentical()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray original =
        "<?xml version=\"1.0\"?><catalogue><book id=\"1\"><title>A</title>"
        "<!--note--></book><book id=\"2\"><title>B</title></book></catalogue>";
    const QString path = writeTempFile(dir, "in.xml", original);
    QVERIFY(!path.isEmpty());

    // Open through the real mmap path and beautify.
    MmapBuffer buf;
    QVERIFY(buf.open(path.toUtf8().constData()));
    PieceTable src(&buf);

    FormatEngine engine;
    FormatEngine::Options opts;
    auto formatted = engine.format(src, opts);
    QVERIFY(formatted != nullptr);
    const std::string expected = dump(*formatted);
    QVERIFY(!expected.empty());

    // Save by streaming the piece list, the way the editor writes files.
    const QString outPath = dir.filePath("out.xml");
    {
        QFile out(outPath);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        auto it = formatted->begin();
        while (!it.atEnd()) {
            const std::string_view chunk = it.nextChunk();
            if (chunk.empty()) continue;
            QCOMPARE(out.write(chunk.data(), static_cast<qint64>(chunk.size())),
                     static_cast<qint64>(chunk.size()));
        }
    }

    // Reopen and compare byte for byte.
    MmapBuffer reopened;
    QVERIFY(reopened.open(outPath.toUtf8().constData()));
    PieceTable roundTripped(&reopened);
    QCOMPARE(roundTripped.length(), static_cast<uint64_t>(expected.size()));
    QCOMPARE(dump(roundTripped), expected);

    // Beautifying the saved file again must change nothing.
    auto again = engine.format(roundTripped, opts);
    QVERIFY(again != nullptr);
    QCOMPARE(dump(*again), expected);
}

void tst_FormatEngine::cancellation_returnsNullptr()
{
    std::string text = "<root>";
    for (int i = 0; i < 20000; ++i) text += "<item>value</item>";
    text += "</root>";

    auto src = doc(text);
    FormatEngine engine;
    FormatEngine::Options opts;
    // Cancel immediately: the very first check must abort the run.
    auto out = engine.format(*src, opts, {}, [] { return true; });
    QVERIFY(out == nullptr);
}

void tst_FormatEngine::progressCallback_calledDuringFormat()
{
    std::string text = "<root>";
    for (int i = 0; i < 200000; ++i) text += "<item>value</item>";
    text += "</root>";

    auto src = doc(text);
    FormatEngine engine;
    FormatEngine::Options opts;

    QVector<int> seen;
    auto out = engine.format(*src, opts, [&seen](int pct) { seen.append(pct); });
    QVERIFY(out != nullptr);
    QVERIFY(!seen.isEmpty());
    QCOMPARE(seen.last(), 100); // always finishes at 100 %
    for (int pct : seen) QVERIFY(pct >= 0 && pct <= 100);
}

void tst_FormatEngine::sinkAbort_returnsFalse()
{
    std::string text = "<root>";
    for (int i = 0; i < 100000; ++i) text += "<item>value</item>";
    text += "</root>";

    auto src = doc(text);
    FormatEngine engine;
    FormatEngine::Options opts;
    QVERIFY(!engine.formatToSink(*src, opts, [](std::string_view) { return false; }));
}

void tst_FormatEngine::emptyDocument_producesNothing()
{
    auto src = doc("");
    FormatEngine engine;
    FormatEngine::Options opts;
    auto out = engine.format(*src, opts);
    QVERIFY(out != nullptr);
    QCOMPARE(out->length(), uint64_t{0});
}

void tst_FormatEngine::throughput_atLeast50MBps()
{
    LOXE_SKIP_IF_SANITIZED();

    // ~8 MB of realistic markup.
    std::string text = "<catalogue>\n";
    while (text.size() < (8u << 20))
        text += "  <book id=\"b\"><title>A Title</title><year>2024</year></book>\n";
    text += "</catalogue>\n";

    auto src = doc(text);
    FormatEngine engine;
    FormatEngine::Options opts;

    QElapsedTimer timer;
    timer.start();
    uint64_t produced = 0;
    const bool ok = engine.formatToSink(*src, opts,
        [&produced](std::string_view block) { produced += block.size(); return true; });
    const qint64 ms = timer.elapsed();

    QVERIFY(ok);
    QVERIFY(produced > 0);
    const double mbps = (static_cast<double>(text.size()) / 1e6) / (std::max<qint64>(1, ms) / 1000.0);
    // SRS PF-08: beautify throughput ≥ 50 MB/s.
    QVERIFY2(mbps >= 50.0, qPrintable(QString("throughput %1 MB/s").arg(mbps, 0, 'f', 1)));
}

void tst_FormatEngine::memoryUsage_outputBufferBounded()
{
    // Streaming to a sink must never hand over a block larger than the declared
    // 8 MB buffer, whatever the document size.
    std::string text = "<root>\n";
    while (text.size() < (24u << 20)) text += "  <item>payload payload payload</item>\n";
    text += "</root>\n";

    auto src = doc(text);
    FormatEngine engine;
    FormatEngine::Options opts;

    size_t largestBlock = 0;
    const bool ok = engine.formatToSink(*src, opts,
        [&largestBlock](std::string_view block) {
            largestBlock = std::max(largestBlock, block.size());
            return true;
        });

    QVERIFY(ok);
    QVERIFY(largestBlock > 0);
    QVERIFY2(largestBlock <= FormatEngine::kOutputBufferSize + (1u << 20),
             qPrintable(QString("largest block %1 bytes").arg(largestBlock)));
}

QTEST_MAIN(tst_FormatEngine)
#include "tst_FormatEngine.moc"
