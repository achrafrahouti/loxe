#include <QtTest>
#include "engine/FormatEngine.h"
#include "engine/PieceTable.h"
#include "ui/VirtualTreeModel.h"
#include "TestHelpers.h"

#include <algorithm>
#include <memory>

using namespace loxe_test;

// Covers the model API the tree context menu depends on: an element's byte
// range ("Parse" / "Copy element") and its location path ("Copy XPath").
class tst_VirtualTreeModel : public QObject {
    Q_OBJECT
private slots:
    void scanRoots_findsDocumentElementAndChildren();
    void scanRoots_skipsPrologue();

    void elementRange_coversWholeElement();
    void elementRange_selfClosingElement();
    void elementRange_nestedSameNameDoesNotCloseEarly();
    void elementRange_onMinifiedDocument();

    void xpath_simplePath();
    void xpath_addsPredicateOnlyWhenNameRepeats();

    void parsePipeline_minifiedElementBecomesMultiLine();

private:
    std::unique_ptr<PieceTable>       m_doc;
    std::unique_ptr<VirtualTreeModel> m_model;

    // Builds a model over `text` with the document element expanded.
    void load(std::string_view text)
    {
        m_doc = std::make_unique<PieceTable>(nullptr);
        m_doc->appendInitial(text);
        m_model = std::make_unique<VirtualTreeModel>(m_doc.get());
        m_model->setInitialNodes(VirtualTreeModel::scanRoots(*m_doc));
    }

    QModelIndex root() const { return m_model->index(0, 0, {}); }

    QModelIndex child(int row, const QModelIndex& parent) const
    {
        if (m_model->canFetchMore(parent)) m_model->fetchMore(parent);
        return m_model->index(row, 0, parent);
    }

    std::string sourceOf(const QModelIndex& index) const
    {
        uint64_t start = 0, end = 0;
        if (!m_model->elementRange(index, &start, &end)) return {};
        return m_doc->read(start, end - start);
    }
};

// --- Root scan ---------------------------------------------------------------

void tst_VirtualTreeModel::scanRoots_findsDocumentElementAndChildren()
{
    load("<r><a/><b/><c/></r>");
    QCOMPARE(m_model->rowCount({}), 1);
    QCOMPARE(m_model->tagNameFor(root()), QStringLiteral("r"));
    QCOMPARE(m_model->rowCount(root()), 3);
    QCOMPARE(m_model->tagNameFor(m_model->index(1, 0, root())), QStringLiteral("b"));
}

void tst_VirtualTreeModel::scanRoots_skipsPrologue()
{
    load("<?xml version=\"1.0\"?><!-- note --><!DOCTYPE r><r><a/></r>");
    QCOMPARE(m_model->tagNameFor(root()), QStringLiteral("r"));
}

// --- Element ranges ----------------------------------------------------------

void tst_VirtualTreeModel::elementRange_coversWholeElement()
{
    load("<r><a>AAA</a><b>BBB</b></r>");
    QCOMPARE(sourceOf(child(0, root())), std::string("<a>AAA</a>"));
    QCOMPARE(sourceOf(child(1, root())), std::string("<b>BBB</b>"));
    QCOMPARE(sourceOf(root()),           std::string("<r><a>AAA</a><b>BBB</b></r>"));
}

void tst_VirtualTreeModel::elementRange_selfClosingElement()
{
    load("<r><a/><b>x</b></r>");
    QCOMPARE(sourceOf(child(0, root())), std::string("<a/>"));
}

void tst_VirtualTreeModel::elementRange_nestedSameNameDoesNotCloseEarly()
{
    // The inner </box> must not be mistaken for the outer element's end tag.
    load("<r><box id=\"1\"><box id=\"2\">deep</box>tail</box></r>");
    QCOMPARE(sourceOf(child(0, root())),
             std::string("<box id=\"1\"><box id=\"2\">deep</box>tail</box>"));
}

void tst_VirtualTreeModel::elementRange_onMinifiedDocument()
{
    // No newlines anywhere: ranges are byte offsets, so layout is irrelevant.
    load("<orders><order id=\"0\"><t>1</t></order><order id=\"1\"><t>2</t></order></orders>");
    QCOMPARE(sourceOf(child(1, root())),
             std::string("<order id=\"1\"><t>2</t></order>"));
}

// --- XPath -------------------------------------------------------------------

void tst_VirtualTreeModel::xpath_simplePath()
{
    load("<r><a><b>x</b></a></r>");
    const QModelIndex a = child(0, root());
    const QModelIndex b = child(0, a);
    QCOMPARE(m_model->xpathFor(root()), QStringLiteral("/r"));
    QCOMPARE(m_model->xpathFor(a),      QStringLiteral("/r/a"));
    QCOMPARE(m_model->xpathFor(b),      QStringLiteral("/r/a/b"));
}

void tst_VirtualTreeModel::xpath_addsPredicateOnlyWhenNameRepeats()
{
    load("<r><a/><b/><b/><b/></r>");
    // 'a' is unique, so no predicate; the three 'b's are numbered from 1.
    QCOMPARE(m_model->xpathFor(child(0, root())), QStringLiteral("/r/a"));
    QCOMPARE(m_model->xpathFor(child(1, root())), QStringLiteral("/r/b[1]"));
    QCOMPARE(m_model->xpathFor(child(2, root())), QStringLiteral("/r/b[2]"));
    QCOMPARE(m_model->xpathFor(child(3, root())), QStringLiteral("/r/b[3]"));
}

// --- The "Parse" pipeline ----------------------------------------------------

void tst_VirtualTreeModel::parsePipeline_minifiedElementBecomesMultiLine()
{
    // What the Parse action does: take the element's range out of a minified
    // document and beautify it, so it can be read over several lines.
    load("<orders><order id=\"7\"><customer>Acme</customer>"
         "<lines><line sku=\"A1\"><qty>2</qty></line></lines></order></orders>");

    const std::string raw = sourceOf(child(0, root()));
    QVERIFY(!raw.empty());
    QCOMPARE(raw.find('\n'), std::string::npos); // one line to begin with

    PieceTable fragment(nullptr);
    fragment.appendInitial(raw);

    FormatEngine engine;
    FormatEngine::Options opts;
    opts.mode = FormatEngine::Mode::Beautify;
    auto formatted = engine.format(fragment, opts);
    QVERIFY(formatted != nullptr);

    const std::string pretty = dump(*formatted);
    QVERIFY2(std::count(pretty.begin(), pretty.end(), '\n') >= 6, pretty.c_str());
    QVERIFY2(pretty.find("\n  <customer>Acme</customer>") != std::string::npos,
             pretty.c_str());
    QVERIFY2(pretty.find("\n      <qty>2</qty>") != std::string::npos, pretty.c_str());

    // Reformatting must not lose content: minifying it again returns the source.
    PieceTable back(nullptr);
    back.appendInitial(pretty);
    opts.mode = FormatEngine::Mode::Minify;
    auto reminified = engine.format(back, opts);
    QVERIFY(reminified != nullptr);
    QCOMPARE(dump(*reminified), raw);
}

QTEST_MAIN(tst_VirtualTreeModel)
#include "tst_VirtualTreeModel.moc"
