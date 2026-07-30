#include <QtTest>
#include "engine/PieceTable.h"
#include "engine/SearchEngine.h"
#include "TestHelpers.h"

#include <memory>

using namespace loxe_test;

class tst_SearchEngine : public QObject {
    Q_OBJECT
private slots:
    void findForward_findsFirstMatch();
    void findForward_fromOffset_skipsEarlierMatches();
    void findForward_noMatch_returnsNotFound();
    void findForward_wrapsAround();
    void findForward_noWrap_stopsAtEnd();
    void findForward_caseInsensitive();
    void findForward_emptyNeedle_returnsNotFound();
    void findForward_needleLongerThanDocument();
    void findForward_matchAcrossWindowBoundary();
    void findForward_matchAcrossPieceBoundary();
    void findBackward_findsPreviousMatch();
    void findBackward_wrapsAround();
    void countAll_countsNonOverlapping();
    void countAll_overlappingPatternCountedOnce();
    void cancellation_stopsSearch();

private:
    static std::unique_ptr<PieceTable> doc(std::string_view text)
    {
        auto pt = std::make_unique<PieceTable>(nullptr);
        pt->appendInitial(text);
        return pt;
    }
};

void tst_SearchEngine::findForward_findsFirstMatch()
{
    auto pt = doc("the quick brown fox");
    QCOMPARE(SearchEngine::findForward(*pt, "quick", 0), uint64_t{4});
    QCOMPARE(SearchEngine::findForward(*pt, "the", 0), uint64_t{0});
}

void tst_SearchEngine::findForward_fromOffset_skipsEarlierMatches()
{
    auto pt = doc("aXaXaX");
    QCOMPARE(SearchEngine::findForward(*pt, "X", 0), uint64_t{1});
    QCOMPARE(SearchEngine::findForward(*pt, "X", 2), uint64_t{3});
    QCOMPARE(SearchEngine::findForward(*pt, "X", 4), uint64_t{5});
}

void tst_SearchEngine::findForward_noMatch_returnsNotFound()
{
    auto pt = doc("abcdef");
    QCOMPARE(SearchEngine::findForward(*pt, "zzz", 0), SearchEngine::kNotFound);
}

void tst_SearchEngine::findForward_wrapsAround()
{
    auto pt = doc("target and then nothing");
    SearchEngine::Options opts;
    opts.wrapAround = true;
    // Starting past the only match, the search must come back around to it.
    QCOMPARE(SearchEngine::findForward(*pt, "target", 10, opts), uint64_t{0});
}

void tst_SearchEngine::findForward_noWrap_stopsAtEnd()
{
    auto pt = doc("target and then nothing");
    SearchEngine::Options opts;
    opts.wrapAround = false;
    QCOMPARE(SearchEngine::findForward(*pt, "target", 10, opts), SearchEngine::kNotFound);
}

void tst_SearchEngine::findForward_caseInsensitive()
{
    auto pt = doc("Hello WORLD");
    SearchEngine::Options opts;
    opts.caseSensitive = false;
    QCOMPARE(SearchEngine::findForward(*pt, "world", 0, opts), uint64_t{6});
    QCOMPARE(SearchEngine::findForward(*pt, "HELLO", 0, opts), uint64_t{0});

    opts.caseSensitive = true;
    QCOMPARE(SearchEngine::findForward(*pt, "world", 0, opts), SearchEngine::kNotFound);
}

void tst_SearchEngine::findForward_emptyNeedle_returnsNotFound()
{
    auto pt = doc("abc");
    QCOMPARE(SearchEngine::findForward(*pt, "", 0), SearchEngine::kNotFound);
    QCOMPARE(SearchEngine::countAll(*pt, ""), uint64_t{0});
}

void tst_SearchEngine::findForward_needleLongerThanDocument()
{
    auto pt = doc("ab");
    QCOMPARE(SearchEngine::findForward(*pt, "abcdef", 0), SearchEngine::kNotFound);
}

void tst_SearchEngine::findForward_matchAcrossWindowBoundary()
{
    // The scanner works in 1 MB windows; a match straddling the seam is the
    // classic bug, so place one deliberately across it.
    const size_t window = SearchEngine::kWindow;
    std::string text(window - 3, '.');
    text += "NEEDLE";
    text += std::string(1000, '.');

    auto pt = doc(text);
    QCOMPARE(SearchEngine::findForward(*pt, "NEEDLE", 0), static_cast<uint64_t>(window - 3));
    QCOMPARE(SearchEngine::countAll(*pt, "NEEDLE"), uint64_t{1});
}

void tst_SearchEngine::findForward_matchAcrossPieceBoundary()
{
    // A match spanning FILE/ADD pieces must still be found.
    auto pt = std::make_unique<PieceTable>(nullptr);
    pt->appendInitial("hello ");
    pt->insert(pt->length(), "wor");
    pt->insert(pt->length(), "ld!");
    QVERIFY(pt->pieceCount() >= 1);
    QCOMPARE(SearchEngine::findForward(*pt, "world", 0), uint64_t{6});
}

void tst_SearchEngine::findBackward_findsPreviousMatch()
{
    auto pt = doc("aXbXcXd");
    QCOMPARE(SearchEngine::findBackward(*pt, "X", 6), uint64_t{5});
    QCOMPARE(SearchEngine::findBackward(*pt, "X", 5), uint64_t{3});
    QCOMPARE(SearchEngine::findBackward(*pt, "X", 3), uint64_t{1});
}

void tst_SearchEngine::findBackward_wrapsAround()
{
    auto pt = doc("aXbXc");
    SearchEngine::Options opts;
    opts.wrapAround = true;
    // Nothing before offset 1, so wrap to the last match in the document.
    QCOMPARE(SearchEngine::findBackward(*pt, "X", 1, opts), uint64_t{3});
}

void tst_SearchEngine::countAll_countsNonOverlapping()
{
    auto pt = doc("ab ab ab ab");
    QCOMPARE(SearchEngine::countAll(*pt, "ab"), uint64_t{4});
    QCOMPARE(SearchEngine::countAll(*pt, "zz"), uint64_t{0});
}

void tst_SearchEngine::countAll_overlappingPatternCountedOnce()
{
    // "aaaa" contains "aa" twice when counted non-overlapping.
    auto pt = doc("aaaa");
    QCOMPARE(SearchEngine::countAll(*pt, "aa"), uint64_t{2});
}

void tst_SearchEngine::cancellation_stopsSearch()
{
    std::string text(4 * 1024 * 1024, '.');
    text += "NEEDLE";
    auto pt = doc(text);

    std::atomic<bool> cancelled{true};
    QCOMPARE(SearchEngine::findForward(*pt, "NEEDLE", 0, SearchEngine::Options{}, &cancelled),
             SearchEngine::kNotFound);
}

QTEST_MAIN(tst_SearchEngine)
#include "tst_SearchEngine.moc"
