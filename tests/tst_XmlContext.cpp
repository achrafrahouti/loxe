#include <QtTest>
#include "engine/PieceTable.h"
#include "ui/XmlContext.h"
#include "TestHelpers.h"

#include <memory>
#include <string>

using namespace loxe_test;

class tst_XmlContext : public QObject {
    Q_OBJECT
private slots:
    void nesting_yieldsFullPath();
    void afterClosedSibling_excludesIt();
    void closedSiblingsAreNotAncestors();
    void selfClosingElement_isNotAnAncestor();

    // The cases that produced "/br/br/br" instead of an XPath.
    void unclosedHtmlVoidElement_isNotAnAncestor();
    void commentContainingMarkup_isIgnored();
    void cdataContainingMarkup_isIgnored();
    void attributeValueContainingAngleBrackets_isIgnored();

    void declarationAndDoctype_areSkipped();
    void attributesOfInnermostElement_areReported();
    void cursorInsideStartTag_stillReportsAttributes();
    void attributeQuotingVariants();
    void outsideAnyElement_yieldsEmptyPath();
    void deeplyNested_isCappedNotCorrupted();
    void budgetExhausted_marksTruncated();

private:
    // Builds a document from `text`, using '|' to mark the cursor.
    static std::unique_ptr<PieceTable> doc(std::string text, uint64_t* cursor)
    {
        const size_t at = text.find('|');
        if (at == std::string::npos) {
            *cursor = text.size();
        } else {
            text.erase(at, 1);
            *cursor = at;
        }
        auto pt = std::make_unique<PieceTable>(nullptr);
        pt->appendInitial(text);
        return pt;
    }

    static QString pathOf(const std::string& text,
                          uint64_t budget = XmlContext::kDefaultBudget)
    {
        uint64_t cursor = 0;
        auto pt = doc(text, &cursor);
        const auto info = XmlContext::contextAt(*pt, cursor, budget);
        return info.ancestors.isEmpty()
            ? QString() : QStringLiteral("/") + info.ancestors.join(QLatin1Char('/'));
    }

    static XmlContextInfo infoOf(const std::string& text)
    {
        uint64_t cursor = 0;
        auto pt = doc(text, &cursor);
        return XmlContext::contextAt(*pt, cursor);
    }
};

void tst_XmlContext::nesting_yieldsFullPath()
{
    QCOMPARE(pathOf("<a><b><c>te|xt</c></b></a>"), QStringLiteral("/a/b/c"));
}

void tst_XmlContext::afterClosedSibling_excludesIt()
{
    QCOMPARE(pathOf("<r><a>1</a>|<b>2</b></r>"), QStringLiteral("/r"));
}

void tst_XmlContext::closedSiblingsAreNotAncestors()
{
    QCOMPARE(pathOf("<r><a>1</a><b>2</b><c>3|</c></r>"), QStringLiteral("/r/c"));
}

void tst_XmlContext::selfClosingElement_isNotAnAncestor()
{
    QCOMPARE(pathOf("<r><x/><y>hi|</y></r>"), QStringLiteral("/r/y"));
}

// --- The reported breadcrumb failures ---------------------------------------

void tst_XmlContext::unclosedHtmlVoidElement_isNotAnAncestor()
{
    // <br> never closes. A backward scan counted each one as an open ancestor
    // and produced "/html/body/p/br/br".
    QCOMPARE(pathOf("<html><body><p>a<br>b<br>c|</p></body></html>"),
             QStringLiteral("/html/body/p"));
    // The well-formed spelling must behave identically.
    QCOMPARE(pathOf("<html><body><p>a<br/>b<br/>c|</p></body></html>"),
             QStringLiteral("/html/body/p"));
}

void tst_XmlContext::commentContainingMarkup_isIgnored()
{
    QCOMPARE(pathOf("<r><!-- <fake> --><x>hi|</x></r>"), QStringLiteral("/r/x"));
    QCOMPARE(pathOf("<r><x>hi|<!-- </x> --></x></r>"), QStringLiteral("/r/x"));
}

void tst_XmlContext::cdataContainingMarkup_isIgnored()
{
    QCOMPARE(pathOf("<r><![CDATA[<fake>]]><x>hi|</x></r>"), QStringLiteral("/r/x"));
    QCOMPARE(pathOf("<r><x>hi|<![CDATA[</x>]]></x></r>"), QStringLiteral("/r/x"));
}

void tst_XmlContext::attributeValueContainingAngleBrackets_isIgnored()
{
    QCOMPARE(pathOf("<r><x a=\"1>2\"><y>hi|</y></x></r>"), QStringLiteral("/r/x/y"));
    QCOMPARE(pathOf("<r><x a=\"1<2\"><y>hi|</y></x></r>"), QStringLiteral("/r/x/y"));
    QCOMPARE(pathOf("<r><x a='</y>'><y>hi|</y></x></r>"),  QStringLiteral("/r/x/y"));
}

// --- Prologue and attributes -------------------------------------------------

void tst_XmlContext::declarationAndDoctype_areSkipped()
{
    QCOMPARE(pathOf("<?xml version=\"1.0\"?><!DOCTYPE r><r><a>x|</a></r>"),
             QStringLiteral("/r/a"));
}

void tst_XmlContext::attributesOfInnermostElement_areReported()
{
    const auto info = infoOf("<r><item id=\"5\" name=\"z\">te|xt</item></r>");
    QCOMPARE(info.tagName, QStringLiteral("item"));
    QCOMPARE(info.attributes.size(), 2);
    QCOMPARE(info.attributes[0].first,  QStringLiteral("id"));
    QCOMPARE(info.attributes[0].second, QStringLiteral("5"));
    QCOMPARE(info.attributes[1].first,  QStringLiteral("name"));
    QCOMPARE(info.attributes[1].second, QStringLiteral("z"));
}

void tst_XmlContext::cursorInsideStartTag_stillReportsAttributes()
{
    // The start tag straddles the cursor, so it is not complete in the window
    // behind it — the scan has to reach past the cursor to see it whole.
    const auto info = infoOf("<r><item id=\"5\"| name=\"z\">t</item></r>");
    QCOMPARE(info.tagName, QStringLiteral("item"));
    QCOMPARE(info.attributes.size(), 2);
}

void tst_XmlContext::attributeQuotingVariants()
{
    const auto info = infoOf("<r><e a='sing' b=\"doub\" c=bare d>x|</e></r>");
    QCOMPARE(info.tagName, QStringLiteral("e"));
    QCOMPARE(info.attributes.size(), 4);
    QCOMPARE(info.attributes[0].second, QStringLiteral("sing"));
    QCOMPARE(info.attributes[1].second, QStringLiteral("doub"));
    QCOMPARE(info.attributes[2].second, QStringLiteral("bare"));
    QVERIFY(info.attributes[3].second.isEmpty()); // valueless attribute
}

void tst_XmlContext::outsideAnyElement_yieldsEmptyPath()
{
    QVERIFY(pathOf("|<r><a/></r>").isEmpty());
    QVERIFY(pathOf("<r><a/></r>|").isEmpty());
}

void tst_XmlContext::deeplyNested_isCappedNotCorrupted()
{
    std::string text;
    for (int i = 0; i < 400; ++i) text += "<e" + std::to_string(i) + ">";
    text += "|";
    for (int i = 399; i >= 0; --i) text += "</e" + std::to_string(i) + ">";

    uint64_t cursor = 0;
    auto pt = doc(text, &cursor);
    const auto info = XmlContext::contextAt(*pt, cursor);
    // Capped rather than unbounded, and the innermost entries are the real ones.
    QVERIFY(!info.ancestors.isEmpty());
    QVERIFY(info.ancestors.size() <= 128);
    QCOMPARE(info.ancestors.last(), QStringLiteral("e399"));
}

void tst_XmlContext::budgetExhausted_marksTruncated()
{
    // A tiny budget cannot reach the closing tags, so the chain is incomplete
    // and must say so instead of inventing a root.
    std::string text = "<root><mid>";
    text += std::string(64 * 1024, 'x');
    text += "|";
    text += std::string(64 * 1024, 'y');
    text += "</mid></root>";

    uint64_t cursor = 0;
    auto pt = doc(text, &cursor);
    const auto info = XmlContext::contextAt(*pt, cursor, /*budget=*/1024);
    QVERIFY(info.truncated);
    QVERIFY(info.ancestors.isEmpty()); // nothing closed within the budget
}

QTEST_MAIN(tst_XmlContext)
#include "tst_XmlContext.moc"
