#include <QtTest>
#include "engine/PieceTable.h"
#include "engine/XmlScanner.h"
#include "TestHelpers.h"

#include <memory>
#include <vector>

using namespace loxe_test;

namespace {

struct Seen {
    XmlNode::Kind kind;
    uint64_t      offset;
    std::string   raw;
    std::string   name;
};

std::vector<Seen> scan(std::string_view text)
{
    PieceTable pt(nullptr);
    pt.appendInitial(text);

    std::vector<Seen> out;
    XmlScanner::scanAll(pt, [&out](const XmlNode& n) {
        out.push_back({n.kind, n.offset, std::string(n.raw), std::string(n.name)});
        return true;
    });
    return out;
}

} // namespace

class tst_XmlScanner : public QObject {
    Q_OBJECT
private slots:
    void startAndEndTags_reported();
    void emptyTag_reportedAsEmptyTag();
    void textNodes_reportedBetweenTags();
    void comment_reportedVerbatim();
    void cdata_containingMarkup_isOpaque();
    void processingInstruction_reported();
    void doctypeWithInternalSubset_notTerminatedEarly();
    void attributeWithAngleBracket_doesNotEndTag();
    void offsets_pointAtNodeStart();
    void oversizedTextNode_isSplit();
    void callbackStop_endsScan();
    void rangeScan_startsAtOffset();
    void firstAttribute_parsesNameAndValue();
    void firstAttribute_noAttributes_returnsFalse();
    void attributeValue_findsNamedAttribute();
    void attributeValue_singleQuotedAndUnquoted();
    void malformedUnterminatedTag_doesNotHang();
};

void tst_XmlScanner::startAndEndTags_reported()
{
    const auto nodes = scan("<a><b></b></a>");
    QCOMPARE(nodes.size(), size_t{4});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::StartTag);
    QCOMPARE(nodes[0].name, std::string("a"));
    QCOMPARE(nodes[1].kind, XmlNode::Kind::StartTag);
    QCOMPARE(nodes[1].name, std::string("b"));
    QCOMPARE(nodes[2].kind, XmlNode::Kind::EndTag);
    QCOMPARE(nodes[2].name, std::string("b"));
    QCOMPARE(nodes[3].kind, XmlNode::Kind::EndTag);
    QCOMPARE(nodes[3].name, std::string("a"));
}

void tst_XmlScanner::emptyTag_reportedAsEmptyTag()
{
    const auto nodes = scan("<a/><b />");
    QCOMPARE(nodes.size(), size_t{2});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::EmptyTag);
    QCOMPARE(nodes[0].name, std::string("a"));
    QCOMPARE(nodes[1].kind, XmlNode::Kind::EmptyTag);
    QCOMPARE(nodes[1].name, std::string("b"));
}

void tst_XmlScanner::textNodes_reportedBetweenTags()
{
    const auto nodes = scan("<a>hello</a>");
    QCOMPARE(nodes.size(), size_t{3});
    QCOMPARE(nodes[1].kind, XmlNode::Kind::Text);
    QCOMPARE(nodes[1].raw, std::string("hello"));
    QCOMPARE(nodes[1].offset, uint64_t{3});
}

void tst_XmlScanner::comment_reportedVerbatim()
{
    const auto nodes = scan("<a><!-- x <y> z --></a>");
    QCOMPARE(nodes.size(), size_t{3});
    QCOMPARE(nodes[1].kind, XmlNode::Kind::Comment);
    QCOMPARE(nodes[1].raw, std::string("<!-- x <y> z -->"));
}

void tst_XmlScanner::cdata_containingMarkup_isOpaque()
{
    const auto nodes = scan("<a><![CDATA[<b>not a tag</b>]]></a>");
    QCOMPARE(nodes.size(), size_t{3});
    QCOMPARE(nodes[1].kind, XmlNode::Kind::Cdata);
    QCOMPARE(nodes[1].raw, std::string("<![CDATA[<b>not a tag</b>]]>"));
}

void tst_XmlScanner::processingInstruction_reported()
{
    const auto nodes = scan("<?xml version=\"1.0\"?><a/>");
    QCOMPARE(nodes.size(), size_t{2});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::ProcessingInstruction);
    QCOMPARE(nodes[0].raw, std::string("<?xml version=\"1.0\"?>"));
}

void tst_XmlScanner::doctypeWithInternalSubset_notTerminatedEarly()
{
    const auto nodes = scan("<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]><r/>");
    QCOMPARE(nodes.size(), size_t{2});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::Doctype);
    QCOMPARE(nodes[0].raw, std::string("<!DOCTYPE r [<!ELEMENT r (#PCDATA)>]>"));
    QCOMPARE(nodes[1].kind, XmlNode::Kind::EmptyTag);
}

void tst_XmlScanner::attributeWithAngleBracket_doesNotEndTag()
{
    const auto nodes = scan("<a t=\"x > y\"/>");
    QCOMPARE(nodes.size(), size_t{1});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::EmptyTag);
    QCOMPARE(nodes[0].raw, std::string("<a t=\"x > y\"/>"));
}

void tst_XmlScanner::offsets_pointAtNodeStart()
{
    const auto nodes = scan("<aa>tt</aa>");
    QCOMPARE(nodes[0].offset, uint64_t{0});  // <aa>
    QCOMPARE(nodes[1].offset, uint64_t{4});  // tt
    QCOMPARE(nodes[2].offset, uint64_t{6});  // </aa>
}

void tst_XmlScanner::oversizedTextNode_isSplit()
{
    // A single text node larger than kMaxTextNode must arrive in several pieces
    // so scanning stays within bounded memory.
    std::string text = "<a>";
    text += std::string(XmlScanner::kMaxTextNode * 2 + 100, 'x');
    text += "</a>";

    const auto nodes = scan(text);
    int textNodes = 0;
    size_t totalText = 0;
    for (const auto& n : nodes) {
        if (n.kind != XmlNode::Kind::Text) continue;
        ++textNodes;
        totalText += n.raw.size();
        QVERIFY(n.raw.size() <= XmlScanner::kMaxTextNode);
    }
    QVERIFY(textNodes >= 3);
    QCOMPARE(totalText, XmlScanner::kMaxTextNode * 2 + 100);
}

void tst_XmlScanner::callbackStop_endsScan()
{
    PieceTable pt(nullptr);
    pt.appendInitial("<a><b/><c/><d/></a>");

    int count = 0;
    const bool completed = XmlScanner::scanAll(pt, [&count](const XmlNode&) {
        ++count;
        return count < 2; // stop after the second node
    });
    QVERIFY(!completed);
    QCOMPARE(count, 2);
}

void tst_XmlScanner::rangeScan_startsAtOffset()
{
    PieceTable pt(nullptr);
    pt.appendInitial("<a><b>t</b></a>");

    std::vector<std::string> names;
    XmlScanner::scan(pt, 3, pt.length(), [&names](const XmlNode& n) {
        if (n.kind == XmlNode::Kind::StartTag) names.push_back(std::string(n.name));
        return true;
    });
    // Starting at the '<' of <b> must not report <a>.
    QCOMPARE(names.size(), size_t{1});
    QCOMPARE(names[0], std::string("b"));
}

void tst_XmlScanner::firstAttribute_parsesNameAndValue()
{
    std::string_view name, value;
    QVERIFY(XmlScanner::firstAttribute("<a id=\"7\" x=\"1\">", &name, &value));
    QCOMPARE(std::string(name), std::string("id"));
    QCOMPARE(std::string(value), std::string("7"));
}

void tst_XmlScanner::firstAttribute_noAttributes_returnsFalse()
{
    std::string_view name, value;
    QVERIFY(!XmlScanner::firstAttribute("<a>", &name, &value));
    QVERIFY(!XmlScanner::firstAttribute("<a/>", &name, &value));
}

void tst_XmlScanner::attributeValue_findsNamedAttribute()
{
    QCOMPARE(std::string(XmlScanner::attributeValue("<a id=\"7\" cls=\"big\">", "cls")),
             std::string("big"));
    QVERIFY(XmlScanner::attributeValue("<a id=\"7\">", "missing").empty());
}

void tst_XmlScanner::attributeValue_singleQuotedAndUnquoted()
{
    QCOMPARE(std::string(XmlScanner::attributeValue("<a x='v v'>", "x")), std::string("v v"));
    QCOMPARE(std::string(XmlScanner::attributeValue("<a x=bare>", "x")), std::string("bare"));
}

void tst_XmlScanner::malformedUnterminatedTag_doesNotHang()
{
    // Truncated markup must terminate without emitting a bogus node.
    const auto nodes = scan("<a><b unterminated");
    QCOMPARE(nodes.size(), size_t{1});
    QCOMPARE(nodes[0].kind, XmlNode::Kind::StartTag);
    QCOMPARE(nodes[0].name, std::string("a"));
}

QTEST_MAIN(tst_XmlScanner)
#include "tst_XmlScanner.moc"
