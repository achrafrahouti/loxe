// UI smoke tests: drive ViewportRenderer through real key and mouse events and
// assert the document, cursor and selection end up in the right state.
#include <QtTest>
#include <QApplication>
#include <QClipboard>

#include "engine/PieceTable.h"
#include "engine/SparseLineIndex.h"
#include "ui/ViewportRenderer.h"
#include "TestHelpers.h"

#include <atomic>
#include <memory>

using namespace loxe_test;

class tst_ViewportEditing : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void typing_insertsAtCursor();
    void typing_emitsDocumentEdited();
    void enter_insertsNewline();
    void backspace_deletesPreviousCharacter();
    void backspace_atStart_isNoOp();
    void backspace_joinsLines();
    void deleteKey_deletesNextCharacter();
    void deleteKey_atEnd_isNoOp();

    void arrowRight_advancesCursor();
    void arrowLeft_atStart_staysAtZero();
    void arrowDown_movesToNextLine();
    void homeAndEnd_moveWithinLine();
    void ctrlHomeAndEnd_moveToDocumentBounds();
    void arrowDown_keepsStickyColumnThroughShortLine();

    void shiftArrow_extendsSelection();
    void selectAll_selectsWholeDocument();
    void typingOverSelection_replacesIt();
    void typingOverSelection_undoesInOneStep();

    void copyPaste_roundTrips();
    void cut_removesAndCopies();

    void undoRedo_restoresDocumentAndCursor();
    void readOnly_rejectsEdits();

    // Scrolling.
    void wheelScrolls_onAnAttachedButUnscannedIndex();
    void wheelScrolls_afterIndexBuild();

    // Scoped view ("parse this element only").
    void viewRange_clampsCursorToScope();
    void viewRange_rendersOnlyTheScope();
    void viewRange_editInsideScopeAdjustsEnd();
    void viewRange_clearRestoresWholeDocument();
    void viewRange_undoDropsTheScope();

    // Minified documents (one enormous line).
    void flatDocument_reportsSingleLine();
    void flatDocument_cursorEndStaysOnLineOne();

    void utf8_cursorMovesByCharacterNotByte();
    void utf8_backspaceDeletesWholeCharacter();

private:
    void setDoc(std::string_view text);
    std::string text() const { return m_table->read(0, m_table->length()); }
    void key(Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::keyClick(m_viewport.get(), k, mods);
    }
    void type(const QString& s) { QTest::keyClicks(m_viewport.get(), s); }

    std::unique_ptr<PieceTable>       m_table;
    std::unique_ptr<SparseLineIndex>  m_index;
    std::unique_ptr<ViewportRenderer> m_viewport;
};

void tst_ViewportEditing::init()
{
    m_viewport = std::make_unique<ViewportRenderer>();
    m_viewport->resize(800, 600);
    m_viewport->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_viewport.get()));
}

void tst_ViewportEditing::cleanup()
{
    m_viewport.reset();
    m_index.reset();
    m_table.reset();
}

void tst_ViewportEditing::setDoc(std::string_view content)
{
    m_table = std::make_unique<PieceTable>(nullptr);
    m_table->appendInitial(content);
    m_table->clearUndo();
    m_index = std::make_unique<SparseLineIndex>();
    m_index->attach(*m_table);
    m_viewport->setDocument(m_table.get(), m_index.get());
    m_viewport->setFocus();
}

// --- Insertion ---

void tst_ViewportEditing::typing_insertsAtCursor()
{
    setDoc("<a></a>");
    m_viewport->setCursorOffset(3);
    type(QStringLiteral("hi"));
    QCOMPARE(text(), std::string("<a>hi</a>"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{5});
}

void tst_ViewportEditing::typing_emitsDocumentEdited()
{
    setDoc("<a/>");
    QSignalSpy spy(m_viewport.get(), &ViewportRenderer::documentEdited);
    m_viewport->setCursorOffset(0);
    type(QStringLiteral("x"));
    QCOMPARE(spy.count(), 1);
}

void tst_ViewportEditing::enter_insertsNewline()
{
    setDoc("ab");
    m_viewport->setCursorOffset(1);
    key(Qt::Key_Return);
    QCOMPARE(text(), std::string("a\nb"));
    QCOMPARE(m_viewport->cursorLine(), uint64_t{1});
}

void tst_ViewportEditing::backspace_deletesPreviousCharacter()
{
    setDoc("abc");
    m_viewport->setCursorOffset(2);
    key(Qt::Key_Backspace);
    QCOMPARE(text(), std::string("ac"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{1});
}

void tst_ViewportEditing::backspace_atStart_isNoOp()
{
    setDoc("abc");
    m_viewport->setCursorOffset(0);
    key(Qt::Key_Backspace);
    QCOMPARE(text(), std::string("abc"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{0});
}

void tst_ViewportEditing::backspace_joinsLines()
{
    setDoc("ab\ncd");
    m_viewport->setCursorOffset(3); // start of the second line
    key(Qt::Key_Backspace);
    QCOMPARE(text(), std::string("abcd"));
}

void tst_ViewportEditing::deleteKey_deletesNextCharacter()
{
    setDoc("abc");
    m_viewport->setCursorOffset(1);
    key(Qt::Key_Delete);
    QCOMPARE(text(), std::string("ac"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{1});
}

void tst_ViewportEditing::deleteKey_atEnd_isNoOp()
{
    setDoc("abc");
    m_viewport->setCursorOffset(3);
    key(Qt::Key_Delete);
    QCOMPARE(text(), std::string("abc"));
}

// --- Navigation ---

void tst_ViewportEditing::arrowRight_advancesCursor()
{
    setDoc("abc");
    m_viewport->setCursorOffset(0);
    key(Qt::Key_Right);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{1});
    key(Qt::Key_Right);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{2});
}

void tst_ViewportEditing::arrowLeft_atStart_staysAtZero()
{
    setDoc("abc");
    m_viewport->setCursorOffset(0);
    key(Qt::Key_Left);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{0});
}

void tst_ViewportEditing::arrowDown_movesToNextLine()
{
    setDoc("aaa\nbbb\nccc");
    m_viewport->setCursorOffset(1); // line 0, col 1
    key(Qt::Key_Down);
    QCOMPARE(m_viewport->cursorLine(), uint64_t{1});
    QCOMPARE(m_viewport->cursorColumn(), 1);
    key(Qt::Key_Down);
    QCOMPARE(m_viewport->cursorLine(), uint64_t{2});
}

void tst_ViewportEditing::homeAndEnd_moveWithinLine()
{
    setDoc("aaa\nbbbbb\nccc");
    m_viewport->setCursorOffset(6); // inside line 1
    key(Qt::Key_Home);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{4});
    key(Qt::Key_End);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{9});
}

void tst_ViewportEditing::ctrlHomeAndEnd_moveToDocumentBounds()
{
    setDoc("aaa\nbbb\nccc");
    m_viewport->setCursorOffset(5);
    key(Qt::Key_Home, Qt::ControlModifier);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{0});
    key(Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(m_viewport->cursorOffset(), m_table->length());
}

void tst_ViewportEditing::arrowDown_keepsStickyColumnThroughShortLine()
{
    // Travelling down through a short line and back out must restore the
    // original column rather than clamping permanently.
    setDoc("aaaaaaaa\nbb\ncccccccc");
    m_viewport->setCursorOffset(6); // line 0, col 6
    QCOMPARE(m_viewport->cursorColumn(), 6);

    key(Qt::Key_Down);
    QCOMPARE(m_viewport->cursorLine(), uint64_t{1});
    QCOMPARE(m_viewport->cursorColumn(), 2); // clamped to the short line

    key(Qt::Key_Down);
    QCOMPARE(m_viewport->cursorLine(), uint64_t{2});
    QCOMPARE(m_viewport->cursorColumn(), 6); // sticky column restored
}

// --- Selection ---

void tst_ViewportEditing::shiftArrow_extendsSelection()
{
    setDoc("abcdef");
    m_viewport->setCursorOffset(1);
    key(Qt::Key_Right, Qt::ShiftModifier);
    key(Qt::Key_Right, Qt::ShiftModifier);
    QVERIFY(m_viewport->hasSelection());
    QCOMPARE(m_viewport->selectionStart(), uint64_t{1});
    QCOMPARE(m_viewport->selectionEnd(), uint64_t{3});
    QCOMPARE(m_viewport->selectedText(), QStringLiteral("bc"));
}

void tst_ViewportEditing::selectAll_selectsWholeDocument()
{
    setDoc("<a>x</a>");
    key(Qt::Key_A, Qt::ControlModifier);
    QVERIFY(m_viewport->hasSelection());
    QCOMPARE(m_viewport->selectionStart(), uint64_t{0});
    QCOMPARE(m_viewport->selectionEnd(), m_table->length());
}

void tst_ViewportEditing::typingOverSelection_replacesIt()
{
    setDoc("abcdef");
    m_viewport->selectRange(1, 4); // "bcd"
    type(QStringLiteral("Z"));
    QCOMPARE(text(), std::string("aZef"));
    QVERIFY(!m_viewport->hasSelection());
}

void tst_ViewportEditing::typingOverSelection_undoesInOneStep()
{
    // Replacing a selection is delete+insert but must be a single undo step.
    setDoc("abcdef");
    m_viewport->selectRange(1, 4);
    type(QStringLiteral("Z"));
    QCOMPARE(text(), std::string("aZef"));

    m_viewport->undo();
    QCOMPARE(text(), std::string("abcdef"));
    QVERIFY(!m_table->canUndo());
}

void tst_ViewportEditing::copyPaste_roundTrips()
{
    setDoc("hello world");
    m_viewport->selectRange(0, 5);
    key(Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello"));

    m_viewport->setCursorOffset(m_table->length());
    key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(text(), std::string("hello worldhello"));
}

void tst_ViewportEditing::cut_removesAndCopies()
{
    setDoc("hello world");
    m_viewport->selectRange(0, 6);
    key(Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello "));
    QCOMPARE(text(), std::string("world"));
}

// --- Undo / read-only ---

void tst_ViewportEditing::undoRedo_restoresDocumentAndCursor()
{
    setDoc("<a></a>");
    m_viewport->setCursorOffset(3);
    type(QStringLiteral("xy"));
    QCOMPARE(text(), std::string("<a>xy</a>"));

    // Each keystroke is its own undo step, newest first.
    m_viewport->undo();
    QCOMPARE(text(), std::string("<a>x</a>"));
    m_viewport->undo();
    QCOMPARE(text(), std::string("<a></a>"));
    QVERIFY(!m_table->canUndo());

    m_viewport->redo();
    QCOMPARE(text(), std::string("<a>x</a>"));
    m_viewport->redo();
    QCOMPARE(text(), std::string("<a>xy</a>"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{5});
}

void tst_ViewportEditing::readOnly_rejectsEdits()
{
    setDoc("<a/>");
    m_viewport->setReadOnly(true);
    m_viewport->setCursorOffset(0);

    type(QStringLiteral("x"));
    key(Qt::Key_Return);
    key(Qt::Key_Delete);
    m_viewport->setCursorOffset(2);
    key(Qt::Key_Backspace);

    QCOMPARE(text(), std::string("<a/>"));
    QVERIFY(!m_table->canUndo());
}

// --- Scrolling ---

void tst_ViewportEditing::wheelScrolls_onAnAttachedButUnscannedIndex()
{
    // attach() leaves the index unscanned, so estimatedLineCount() is 1 until
    // something walks the document. The scroll bar range must not be frozen at
    // that estimate, or the wheel silently does nothing while the keyboard —
    // which moves the view directly — still works.
    std::string text;
    for (int i = 0; i < 400; ++i) text += "<line n=\"" + std::to_string(i) + "\"/>\n";
    setDoc(text);   // index is attached, never built

    QCOMPARE(m_viewport->firstVisibleLine(), uint64_t{0});

    QWheelEvent wheel(QPointF(50, 50), m_viewport->mapToGlobal(QPoint(50, 50)),
                      QPoint(0, 0), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_viewport.get(), &wheel);

    QVERIFY2(m_viewport->firstVisibleLine() > 0,
             "mouse wheel did not scroll the view");
}

void tst_ViewportEditing::wheelScrolls_afterIndexBuild()
{
    std::string text;
    for (int i = 0; i < 400; ++i) text += "<line n=\"" + std::to_string(i) + "\"/>\n";
    setDoc(text);

    std::atomic<bool> cancel{false};
    m_index->build(*m_table, cancel);
    m_viewport->refreshScrollBars();

    QWheelEvent wheel(QPointF(50, 50), m_viewport->mapToGlobal(QPoint(50, 50)),
                      QPoint(0, 0), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_viewport.get(), &wheel);

    const uint64_t afterDown = m_viewport->firstVisibleLine();
    QVERIFY(afterDown > 0);

    // And back up again.
    QWheelEvent up(QPointF(50, 50), m_viewport->mapToGlobal(QPoint(50, 50)),
                   QPoint(0, 0), QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_viewport.get(), &up);
    QCOMPARE(m_viewport->firstVisibleLine(), uint64_t{0});
}

// --- Scoped view ---

void tst_ViewportEditing::viewRange_clampsCursorToScope()
{
    //           0123456789...
    setDoc("<r><a>AAA</a><b>BBB</b><c>CCC</c></r>");
    const std::string all = text();
    const auto bStart = static_cast<uint64_t>(all.find("<b>"));
    const auto bEnd   = static_cast<uint64_t>(all.find("</b>") + 4);

    m_viewport->setViewRange(bStart, bEnd);
    QVERIFY(m_viewport->hasViewRange());
    QCOMPARE(m_viewport->viewRangeStart(), bStart);
    QCOMPARE(m_viewport->viewRangeEnd(), bEnd);

    // The cursor lands at the start of the element and cannot leave the scope.
    QCOMPARE(m_viewport->cursorOffset(), bStart);

    key(Qt::Key_Home, Qt::ControlModifier);
    QCOMPARE(m_viewport->cursorOffset(), bStart);

    key(Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(m_viewport->cursorOffset(), bEnd);

    key(Qt::Key_Right);
    QCOMPARE(m_viewport->cursorOffset(), bEnd); // clamped at the scope end
}

void tst_ViewportEditing::viewRange_rendersOnlyTheScope()
{
    setDoc("<r>\n  <a>AAA</a>\n  <b>BBB</b>\n  <c>CCC</c>\n</r>\n");
    const std::string all = text();
    const auto bStart = static_cast<uint64_t>(all.find("<b>"));
    const auto bEnd   = static_cast<uint64_t>(all.find("</b>") + 4);

    m_viewport->setViewRange(bStart, bEnd);

    // Select everything the scope exposes: it must be exactly the element.
    m_viewport->selectAll();
    QCOMPARE(m_viewport->selectedText(), QStringLiteral("<b>BBB</b>"));
}

void tst_ViewportEditing::viewRange_editInsideScopeAdjustsEnd()
{
    setDoc("<r><a>AAA</a><b>BBB</b></r>");
    const std::string all = text();
    const auto bStart = static_cast<uint64_t>(all.find("<b>"));
    const auto bEnd   = static_cast<uint64_t>(all.find("</b>") + 4);

    m_viewport->setViewRange(bStart, bEnd);
    m_viewport->setCursorOffset(bStart + 3); // just inside <b>
    type(QStringLiteral("XY"));

    // The element grew by two bytes, so the scope must grow with it or its
    // closing tag would fall outside the view.
    QCOMPARE(m_viewport->viewRangeEnd(), bEnd + 2);
    m_viewport->selectAll();
    QCOMPARE(m_viewport->selectedText(), QStringLiteral("<b>XYBBB</b>"));

    // The edit landed in the real document, not a detached copy.
    QCOMPARE(text(), std::string("<r><a>AAA</a><b>XYBBB</b></r>"));
}

void tst_ViewportEditing::viewRange_clearRestoresWholeDocument()
{
    setDoc("<r><a>AAA</a><b>BBB</b></r>");
    const std::string all = text();
    m_viewport->setViewRange(static_cast<uint64_t>(all.find("<b>")),
                             static_cast<uint64_t>(all.find("</b>") + 4));
    QVERIFY(m_viewport->hasViewRange());

    m_viewport->clearViewRange();
    QVERIFY(!m_viewport->hasViewRange());
    m_viewport->selectAll();
    QCOMPARE(m_viewport->selectedText().toStdString(), all);
}

void tst_ViewportEditing::viewRange_undoDropsTheScope()
{
    // Undo can change the document arbitrarily, so a byte range can no longer
    // be trusted to bound the same element.
    setDoc("<r><b>BBB</b></r>");
    const std::string all = text();
    m_viewport->setViewRange(static_cast<uint64_t>(all.find("<b>")),
                             static_cast<uint64_t>(all.find("</b>") + 4));

    m_viewport->setCursorOffset(m_viewport->viewRangeStart() + 3);
    type(QStringLiteral("Z"));
    QVERIFY(m_viewport->hasViewRange());

    m_viewport->undo();
    QVERIFY(!m_viewport->hasViewRange());
    QCOMPARE(text(), all);
}

// --- Minified documents ---

void tst_ViewportEditing::flatDocument_reportsSingleLine()
{
    // A minified file has no newlines. The viewport must not chop it into
    // pseudo-rows with invented line numbers: it is one line, and the rest is
    // reached by scrolling horizontally.
    std::string text = "<root>";
    text += std::string(512u * 1024, 'x');   // far wider than any viewport
    text += "</root>";
    setDoc(text);

    QCOMPARE(m_index->lineCount(), uint64_t{1});
    QCOMPARE(m_viewport->cursorLine(), uint64_t{0});

    // Column 0 of line 0 is byte 0 whatever the line length.
    m_viewport->setCursorOffset(0);
    QCOMPARE(m_viewport->cursorColumn(), 0);
}

void tst_ViewportEditing::flatDocument_cursorEndStaysOnLineOne()
{
    std::string text(256u * 1024, 'y');
    setDoc(text);

    key(Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(m_viewport->cursorOffset(), m_table->length());
    QCOMPARE(m_viewport->cursorLine(), uint64_t{0});

    // Down on the only line clamps to the end rather than inventing a line.
    key(Qt::Key_Down);
    QCOMPARE(m_viewport->cursorLine(), uint64_t{0});
    QCOMPARE(m_viewport->cursorOffset(), m_table->length());
}

// --- UTF-8 ---

void tst_ViewportEditing::utf8_cursorMovesByCharacterNotByte()
{
    // "é" is two bytes; one arrow press must cross the whole character.
    setDoc("a\xC3\xA9z");
    m_viewport->setCursorOffset(1);
    key(Qt::Key_Right);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{3});
    key(Qt::Key_Left);
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{1});
}

void tst_ViewportEditing::utf8_backspaceDeletesWholeCharacter()
{
    setDoc("a\xC3\xA9z");
    m_viewport->setCursorOffset(3); // just after "é"
    key(Qt::Key_Backspace);
    QCOMPARE(text(), std::string("az"));
    QCOMPARE(m_viewport->cursorOffset(), uint64_t{1});
}

QTEST_MAIN(tst_ViewportEditing)
#include "tst_ViewportEditing.moc"
