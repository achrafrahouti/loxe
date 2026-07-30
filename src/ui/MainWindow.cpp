#include "MainWindow.h"
#include "AsyncLoader.h"
#include "FindBar.h"
#include "ViewportRenderer.h"
#include "VirtualTreeModel.h"
#include "XmlContext.h"
#include "../engine/Encoding.h"
#include "../engine/FormatEngine.h"
#include "../engine/MmapBuffer.h"
#include "../engine/PieceTable.h"
#include "../engine/SearchEngine.h"
#include "../engine/SparseLineIndex.h"
#include "../engine/Validator.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QClipboard>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

constexpr int      kMaxRecentFiles   = 20;
constexpr int      kValidationDelayMs = 500;
// Above this size the document no longer fits comfortably in memory, which
// rules out an in-memory reformat or a QString-based regex search. Validation
// is exempt: libxml2 streams it.
constexpr uint64_t kWholeDocumentLimit = 256ull * 1024 * 1024;
// Copying more than this to the clipboard is confirmed first.
constexpr uint64_t kMaxClipboardBytes = 32ull * 1024 * 1024;
// "Parse" copies and reformats the element in memory, so it is capped well
// below the whole-document limit.
constexpr uint64_t kMaxParseBytes = 64ull * 1024 * 1024;

QString elide(const QString& s, int maxChars)
{
    return s.size() <= maxChars ? s : s.left(maxChars - 1) + QChar(0x2026);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setupUi();
    setupMenus();
    setupStatusBar();
    connectSignals();
    restoreSession();
    setAcceptDrops(true);

    m_validateTimer = new QTimer(this);
    m_validateTimer->setSingleShot(true);
    m_validateTimer->setInterval(kValidationDelayMs);
    connect(m_validateTimer, &QTimer::timeout, this, &MainWindow::onValidationTimeout);

    m_validateWatcher = new QFutureWatcher<ValidationResult>(this);
    connect(m_validateWatcher, &QFutureWatcher<ValidationResult>::finished,
            this, &MainWindow::onValidationFinished);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &MainWindow::onFileChangedOnDisk);

    newDocument();
}

MainWindow::~MainWindow()
{
    // The worker holds a raw PieceTable pointer, so it must finish before the
    // document is destroyed.
    cancelValidation();
    if (m_validateWatcher) m_validateWatcher->waitForFinished();
}

// --- Document lifecycle ---

void MainWindow::openFile(const QString& path)
{
    if (path.isEmpty()) return;
    if (!confirmDiscardChanges()) return;

    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        QMessageBox::critical(this, tr("Open failed"),
                              tr("No such file: %1").arg(path));
        return;
    }

    if (!m_loader) {
        m_loader = new AsyncLoader(this);
        connect(m_loader, &AsyncLoader::fileReady,    this, &MainWindow::onFileReady);
        connect(m_loader, &AsyncLoader::indexReady,   this, &MainWindow::onIndexReady);
        connect(m_loader, &AsyncLoader::treeReady,    this, &MainWindow::onTreeReady);
        connect(m_loader, &AsyncLoader::loadProgress, this, &MainWindow::onLoadProgress);
        connect(m_loader, &AsyncLoader::loadFailed,   this, &MainWindow::onLoadFailed);
    }

    m_currentPath = path;
    m_isReadOnly  = !fi.isWritable();
    m_viewport->setReadOnly(m_isReadOnly);
    setDirty(false);
    updateWindowTitle();
    setProgressVisible(true);
    m_treeView->setModel(nullptr);
    m_loader->load(path);
}

void MainWindow::newDocument()
{
    if (!confirmDiscardChanges()) return;

    if (m_loader) m_loader->cancel();
    cancelValidation();
    m_validateWatcher->waitForFinished();
    exitPreview();

    // Order matters: the viewport must drop its pointers before the objects die.
    m_viewport->setDocument(nullptr, nullptr);
    m_treeView->setModel(nullptr);

    m_mmapBuf.reset();
    m_pieceTable = std::make_unique<PieceTable>(nullptr);
    m_pieceTable->appendInitial("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root>\n</root>\n");
    m_pieceTable->clearUndo();

    m_lineIndex = std::make_unique<SparseLineIndex>();
    m_lineIndex->attach(*m_pieceTable);

    m_viewport->setDocument(m_pieceTable.get(), m_lineIndex.get());
    m_currentPath.clear();
    m_isReadOnly = false;
    m_viewport->setReadOnly(false);
    setDirty(false);
    updateWindowTitle();
    updateEditActions();
    m_statusEncoding->setText(QStringLiteral("UTF-8"));
    scheduleValidation();
    m_viewport->setFocus();
}

void MainWindow::onFileReady(MmapBuffer* buf, PieceTable* table, SparseLineIndex* index)
{
    // A check may still be running against the outgoing document.
    cancelValidation();
    m_validateWatcher->waitForFinished();
    exitPreview();

    // Detach the viewport before the previous document is destroyed.
    m_viewport->setDocument(nullptr, nullptr);

    m_mmapBuf.reset(buf);
    m_pieceTable.reset(table);
    m_lineIndex.reset(index);

    // The index extends itself lazily, so the first screenful paints now rather
    // than after the full scan (SRS: time to first visible content < 1 s).
    m_viewport->setDocument(table, index);

    const Encoding::Info enc = Encoding::detect(*table);
    m_statusEncoding->setText(QString::fromStdString(enc.name)
                              + (enc.hasBom ? tr(" (BOM)") : QString()));

    updateEditActions();
    watchCurrentFile();
    scheduleValidation();
    m_viewport->setFocus();
}

void MainWindow::onIndexReady()
{
    // Scroll bar range and line count are exact from here on.
    m_viewport->refreshScrollBars();
    statusBar()->showMessage(tr("%1 lines").arg(m_lineIndex->lineCount()), 4000);
}

void MainWindow::onTreeReady(VirtualTreeModel* model)
{
    QAbstractItemModel* old = m_treeView->model();
    m_treeView->setModel(model);
    model->setParent(m_treeView);
    if (old) old->deleteLater();

    m_treeView->setColumnWidth(0, 160);
    m_treeView->setColumnWidth(1, 120);
    m_treeView->expandToDepth(0);
    setProgressVisible(false);
}

void MainWindow::onLoadProgress(int percent, QString phase)
{
    m_progress->setValue(percent);
    statusBar()->showMessage(phase);
}

void MainWindow::onLoadFailed(QString reason)
{
    setProgressVisible(false);
    QMessageBox::critical(this, tr("Open failed"), reason);
}

// --- Protected events ---

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!confirmDiscardChanges()) { e->ignore(); return; }
    saveSession();
    if (m_loader) m_loader->cancel();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    const auto urls = e->mimeData()->urls();
    if (!urls.isEmpty()) openFile(urls.first().toLocalFile());
}

// --- File menu ---

void MainWindow::onFileNew() { newDocument(); }

void MainWindow::onFileOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open XML File"), {},
        tr("XML Files (*.xml *.xhtml *.xsd *.xsl *.svg);;All Files (*)"));
    if (!path.isEmpty()) openFile(path);
}

bool MainWindow::onFileSave()
{
    if (m_currentPath.isEmpty()) return onFileSaveAs();
    if (m_isReadOnly) {
        QMessageBox::warning(this, tr("Read-only"),
                             tr("%1 is not writable. Use Save As.").arg(m_currentPath));
        return false;
    }

    QString error;
    if (!writeDocumentTo(m_currentPath, &error)) {
        QMessageBox::critical(this, tr("Save failed"), error);
        return false;
    }

    setDirty(false);
    statusBar()->showMessage(tr("Saved %1").arg(m_currentPath), 3000);
    return true;
}

bool MainWindow::onFileSaveAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save As"), m_currentPath, tr("XML Files (*.xml);;All Files (*)"));
    if (path.isEmpty()) return false;

    QString error;
    if (!writeDocumentTo(path, &error)) {
        QMessageBox::critical(this, tr("Save failed"), error);
        return false;
    }

    m_currentPath = path;
    m_isReadOnly  = false;
    m_viewport->setReadOnly(false);
    setDirty(false);
    updateWindowTitle();
    updateRecentFiles(path);
    watchCurrentFile();
    statusBar()->showMessage(tr("Saved %1").arg(path), 3000);
    return true;
}

// Atomic save (FIO-06): QSaveFile writes to a temp file in the same directory
// and rename()s it into place on commit, so an interrupted write cannot leave a
// truncated document behind.
bool MainWindow::writeDocumentTo(const QString& path, QString* error)
{
    if (!m_pieceTable) { *error = tr("No document to save."); return false; }

    // Suppress the watcher notification our own write is about to trigger.
    m_ignoreNextWatchEvent = true;

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = out.errorString();
        return false;
    }

    // Stream through the piece list; the document is never assembled in memory.
    auto it = m_pieceTable->begin();
    while (!it.atEnd()) {
        const std::string_view chunk = it.nextChunk();
        if (chunk.empty()) continue;
        if (out.write(chunk.data(), static_cast<qint64>(chunk.size()))
            != static_cast<qint64>(chunk.size())) {
            *error = out.errorString();
            return false;
        }
    }

    if (!out.commit()) { *error = out.errorString(); return false; }
    return true;
}

void MainWindow::onFileRecentTriggered()
{
    if (auto* action = qobject_cast<QAction*>(sender()))
        openFile(action->data().toString());
}

// --- Edit menu ---

void MainWindow::onEditUndo() { m_viewport->undo(); updateEditActions(); }
void MainWindow::onEditRedo() { m_viewport->redo(); updateEditActions(); }

void MainWindow::onEditFind()
{
    m_findBar->activate(false, m_viewport->selectedText());
}

void MainWindow::onEditFindReplace()
{
    m_findBar->activate(true, m_viewport->selectedText());
}

void MainWindow::onEditGoToLine()
{
    SparseLineIndex* index = activeIndex();
    if (!index) return;
    const auto total = static_cast<int>(std::min<uint64_t>(index->lineCount(), INT32_MAX));
    bool ok = false;
    const int line = QInputDialog::getInt(
        this, tr("Go to Line"), tr("Line (1–%1):").arg(total),
        static_cast<int>(m_viewport->cursorLine() + 1), 1, std::max(1, total), 1, &ok);
    if (ok) gotoLine(static_cast<uint64_t>(line - 1));
}

void MainWindow::gotoLine(uint64_t line)
{
    SparseLineIndex* index = activeIndex();
    if (!index) return;
    m_viewport->setCursorOffset(index->lineToOffset(line));
    m_viewport->setFocus();
}

// --- Format menu ---

void MainWindow::onFormatBeautify() { runFormat(false); }
void MainWindow::onFormatMinify()   { runFormat(true); }

void MainWindow::runFormat(bool minify)
{
    if (!m_pieceTable) return;
    exitPreview();
    if (m_isReadOnly) {
        QMessageBox::warning(this, tr("Read-only"), tr("This document is read-only."));
        return;
    }

    const uint64_t len = m_pieceTable->length();
    if (len == 0) return;
    if (len > kWholeDocumentLimit) {
        QMessageBox::warning(this, minify ? tr("Minify") : tr("Beautify"),
            tr("This document is %1 MB. Reformatting keeps the result in memory "
               "as a single undo step, which needs roughly twice that. Split the "
               "file or raise the limit before reformatting.")
                .arg(len / (1024 * 1024)));
        return;
    }

    FormatEngine::Options opts;
    opts.mode = minify ? FormatEngine::Mode::Minify : FormatEngine::Mode::Beautify;

    QProgressDialog dialog(minify ? tr("Minifying…") : tr("Beautifying…"),
                           tr("Cancel"), 0, 100, this);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(300);

    std::string result;
    result.reserve(static_cast<size_t>(len));

    FormatEngine engine;
    const bool ok = engine.formatToSink(
        *m_pieceTable, opts,
        [&result](std::string_view block) { result.append(block); return true; },
        [&dialog](int pct) {
            dialog.setValue(pct);
            QApplication::processEvents();
        },
        [&dialog] { return dialog.wasCanceled(); });

    dialog.close();
    if (!ok) {
        statusBar()->showMessage(tr("Reformat cancelled"), 3000);
        return;
    }

    // A single replaceAll is one undo step for the whole operation.
    const uint64_t cursor = m_viewport->cursorOffset();
    m_pieceTable->replaceAll(result);
    m_lineIndex->invalidateFrom(0);
    m_viewport->setCursorOffset(std::min(cursor, m_pieceTable->length()));
    m_viewport->clearSelection();
    m_viewport->update();
    setDirty(true);
    updateEditActions();
    scheduleValidation();
    statusBar()->showMessage(
        minify ? tr("Minified to %1 bytes").arg(result.size())
               : tr("Beautified to %1 bytes").arg(result.size()), 4000);
}

// --- View menu ---

void MainWindow::onViewToggleWordWrap()
{
    m_viewport->setWordWrap(!m_viewport->wordWrap());
    m_wordWrapAction->setChecked(m_viewport->wordWrap());
}

void MainWindow::onViewToggleTreePane() { m_treeView->setVisible(!m_treeView->isVisible()); }
void MainWindow::onViewToggleAttrPane() { m_attrDock->setVisible(!m_attrDock->isVisible()); }

void MainWindow::onViewToggleDarkTheme()
{
    const bool on = m_darkAction->isChecked();
    m_viewport->setDarkTheme(on);
}

// --- Sync between panes ---

void MainWindow::onCursorMoved(uint64_t byteOffset)
{
    SparseLineIndex* index = activeIndex();
    if (!index) return;

    const uint64_t line = index->offsetToLine(byteOffset);
    m_statusPos->setText(tr("Ln %1, Col %2")
        .arg(line + 1)
        .arg(m_viewport->cursorColumn() + 1));

    updateContextPanels(byteOffset);
    updateEditActions();

    // Offsets in a preview refer to the reformatted copy, not the file, so
    // there is nothing meaningful to select in the tree.
    if (previewActive()) return;

    // Follow the caret in the tree, but only among nodes already loaded — this
    // runs on every cursor movement and must never trigger a document scan.
    if (auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model())) {
        const QModelIndex match = model->indexForOffset(byteOffset);
        if (match.isValid() && match != m_treeView->currentIndex()) {
            QSignalBlocker block(m_treeView); // don't bounce back into the viewport
            m_treeView->setCurrentIndex(match);
            m_treeView->scrollTo(match, QAbstractItemView::EnsureVisible);
        }
    }
}

void MainWindow::onDocumentEdited(uint64_t offset)
{
    Q_UNUSED(offset);
    setDirty(true);
    updateEditActions();
    scheduleValidation();
}

void MainWindow::updateContextPanels(uint64_t byteOffset)
{
    PieceTable* doc = activeDocument();
    if (!doc) return;

    const XmlContextInfo ctx = XmlContext::contextAt(*doc, byteOffset);

    // Breadcrumb: an XPath-ish path from the root to the current element.
    QString path = ctx.ancestors.isEmpty() ? QString()
                                           : QStringLiteral("/") + ctx.ancestors.join(QLatin1Char('/'));
    if (ctx.truncated && !path.isEmpty()) path.prepend(QChar(0x2026));
    m_breadcrumb->setText(elide(path, 300));

    // Attribute panel for the innermost element.
    m_attrTable->setRowCount(ctx.attributes.size());
    for (int i = 0; i < ctx.attributes.size(); ++i) {
        const auto& [name, value] = ctx.attributes.at(i);
        m_attrTable->setItem(i, 0, new QTableWidgetItem(name));
        m_attrTable->setItem(i, 1, new QTableWidgetItem(value));
    }
    m_attrDock->setWindowTitle(ctx.tagName.isEmpty()
        ? tr("Attributes") : tr("Attributes — <%1>").arg(ctx.tagName));
}

void MainWindow::onTreeNodeActivated(const QModelIndex& index)
{
    const auto* model = qobject_cast<const VirtualTreeModel*>(m_treeView->model());
    if (!model) return;
    m_viewport->setCursorOffset(model->byteOffsetFor(index));
    m_viewport->setFocus();
}

// --- Tree context menu ---

void MainWindow::onTreeContextMenu(const QPoint& pos)
{
    auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model());
    if (!model) return;

    const QModelIndex index = m_treeView->indexAt(pos);
    if (!index.isValid()) return;

    m_contextIndex = index;
    m_treeView->setCurrentIndex(index);

    const QString name = model->tagNameFor(index);

    QMenu menu(this);
    menu.addAction(tr("Parse <%1> — show it formatted").arg(name),
                   this, &MainWindow::onTreeParseElement);
    menu.addAction(tr("Show only <%1> (raw, editable)").arg(name),
                   this, &MainWindow::onTreeShowRawElement);
    menu.addAction(tr("Copy <%1> to clipboard").arg(name),
                   this, &MainWindow::onTreeCopyElement);
    menu.addSeparator();
    menu.addAction(tr("Copy XPath"), this, &MainWindow::onTreeCopyXPath);
    menu.addAction(tr("Go to element"), this, [this, index] { onTreeNodeActivated(index); });

    if (m_viewport->hasViewRange() || previewActive()) {
        menu.addSeparator();
        menu.addAction(tr("Show whole document"), this, &MainWindow::onShowWholeDocument);
    }

    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

// Locates the element's byte range, reporting why if it cannot be determined.
bool MainWindow::contextElementRange(uint64_t* start, uint64_t* end)
{
    auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model());
    if (!model || !m_contextIndex.isValid()) return false;

    if (!model->elementRange(QModelIndex(m_contextIndex), start, end)) {
        QMessageBox::warning(this, tr("Element"),
            tr("Could not determine where this element ends. It may be unclosed, "
               "or larger than the scan limit."));
        return false;
    }
    return true;
}

PieceTable* MainWindow::activeDocument() const
{
    return m_previewTable ? m_previewTable.get() : m_pieceTable.get();
}

SparseLineIndex* MainWindow::activeIndex() const
{
    return m_previewTable ? m_previewIndex.get() : m_lineIndex.get();
}

// "Parse": show the element on its own, reformatted.
//
// The element's source is beautified into a separate in-memory document rather
// than reformatted in place, so clicking a menu item called Parse never edits
// the file. That copy is read-only; the real document stays loaded underneath
// and is what save, search, validation and the tree keep working against.
void MainWindow::onTreeParseElement()
{
    uint64_t start = 0, end = 0;
    if (!contextElementRange(&start, &end)) return;

    const uint64_t length = end - start;
    if (length > kMaxParseBytes) {
        QMessageBox::warning(this, tr("Parse element"),
            tr("This element is %1 MB. Reformatting it needs roughly twice that "
               "in memory.\n\nUse \"Show only this element (raw)\" instead — it "
               "renders the element without copying it.")
                .arg(length / (1024 * 1024)));
        return;
    }

    auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model());
    const QString name = model ? model->tagNameFor(QModelIndex(m_contextIndex)) : QString();

    // Extract the element and beautify it. Works whatever the source layout is,
    // so a minified document still comes out indented over multiple lines.
    auto source = std::make_unique<PieceTable>(nullptr);
    source->appendInitial(m_pieceTable->read(start, length));

    FormatEngine engine;
    FormatEngine::Options opts;
    opts.mode = FormatEngine::Mode::Beautify;

    auto formatted = engine.format(*source, opts);
    if (!formatted) {
        QMessageBox::warning(this, tr("Parse element"),
                             tr("Could not reformat <%1>.").arg(name));
        return;
    }

    // Swap the viewport onto the preview. The old preview, if any, must not be
    // freed until the viewport has let go of it.
    m_viewport->setDocument(nullptr, nullptr);

    m_previewTable = std::move(formatted);
    m_previewTable->clearUndo();
    m_previewIndex = std::make_unique<SparseLineIndex>();
    // Scan it now rather than attaching lazily: the preview is capped at 64 MB,
    // so this is quick, and it means the line count — and therefore the scroll
    // bar range — is exact from the very first frame.
    std::atomic<bool> noCancel{false};
    m_previewIndex->build(*m_previewTable, noCancel);
    m_previewName = name;

    m_viewport->setDocument(m_previewTable.get(), m_previewIndex.get());
    m_viewport->setReadOnly(true);   // edits here would go nowhere
    m_viewport->setFocus();

    m_wholeDocAction->setEnabled(true);
    updateWindowTitle();
    updateEditActions();
    statusBar()->showMessage(
        tr("Parsed <%1> — %2 bytes reformatted to %3 lines. "
           "View ▸ Show Whole Document to leave.")
            .arg(name).arg(length).arg(m_previewIndex->lineCount()), 8000);
}

// The editable counterpart: scope the viewport to the element's live bytes.
void MainWindow::onTreeShowRawElement()
{
    uint64_t start = 0, end = 0;
    if (!contextElementRange(&start, &end)) return;

    exitPreview();
    m_viewport->setViewRange(start, end);
    m_wholeDocAction->setEnabled(true);
    m_viewport->setFocus();

    auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model());
    const QString name = model ? model->tagNameFor(QModelIndex(m_contextIndex)) : QString();
    statusBar()->showMessage(
        tr("Showing <%1> only — %2 bytes, still editable. "
           "View ▸ Show Whole Document to leave.").arg(name).arg(end - start), 6000);
    updateWindowTitle();
}

void MainWindow::exitPreview()
{
    if (!m_previewTable) return;

    m_viewport->setDocument(nullptr, nullptr);
    m_previewIndex.reset();
    m_previewTable.reset();
    m_previewName.clear();

    m_viewport->setDocument(m_pieceTable.get(), m_lineIndex.get());
    m_viewport->setReadOnly(m_isReadOnly);
    updateEditActions();
}

void MainWindow::onTreeCopyElement()
{
    uint64_t start = 0, end = 0;
    if (!contextElementRange(&start, &end)) return;

    const uint64_t length = end - start;
    if (length > kMaxClipboardBytes) {
        const auto choice = QMessageBox::question(this, tr("Copy element"),
            tr("This element is %1 MB. Copying it will use at least that much "
               "memory again.\n\nCopy anyway?").arg(length / (1024 * 1024)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
    }

    const std::string raw = m_pieceTable->read(start, length);
    QApplication::clipboard()->setText(
        QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size())));
    statusBar()->showMessage(tr("Copied %1 bytes to the clipboard").arg(length), 4000);
}

void MainWindow::onTreeCopyXPath()
{
    auto* model = qobject_cast<VirtualTreeModel*>(m_treeView->model());
    if (!model || !m_contextIndex.isValid()) return;

    const QString path = model->xpathFor(QModelIndex(m_contextIndex));
    if (path.isEmpty()) return;
    QApplication::clipboard()->setText(path);
    statusBar()->showMessage(tr("Copied %1").arg(path), 4000);
}

void MainWindow::onShowWholeDocument()
{
    exitPreview();
    m_viewport->clearViewRange();
    m_wholeDocAction->setEnabled(false);
    updateWindowTitle();
    statusBar()->showMessage(tr("Showing the whole document"), 3000);
}

// --- Search ---

void MainWindow::searchFor(const QString& term)
{
    m_findBar->activate(false, term);
    onFindNext();
}

bool MainWindow::findFrom(uint64_t from, bool backwards, bool moveCursor)
{
    if (!m_pieceTable) return false;
    // Search covers the whole file, so a match could not be shown inside a
    // preview of one element.
    if (previewActive()) { exitPreview(); from = 0; }

    const QString term = m_findBar->searchTerm();
    if (term.isEmpty()) {
        m_viewport->clearMatchHighlight();
        m_findBar->setStatus({});
        m_lastMatchLength = 0;
        return false;
    }

    uint64_t matchStart  = SearchEngine::kNotFound;
    uint64_t matchLength = 0;

    if (m_findBar->useRegex()) {
        // Regex needs the document as text; bound it to keep this responsive.
        const uint64_t len = m_pieceTable->length();
        if (len > kWholeDocumentLimit) {
            m_findBar->setStatus(tr("Regex needs < 256 MB"), true);
            return false;
        }
        QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
        if (!m_findBar->caseSensitive())
            opts |= QRegularExpression::CaseInsensitiveOption;
        const QRegularExpression re(term, opts);
        if (!re.isValid()) {
            m_findBar->setStatus(tr("Bad pattern"), true);
            return false;
        }

        const std::string raw  = m_pieceTable->read(0, len);
        const QString     text = QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size()));

        // QRegularExpression indexes UTF-16 units while the document is indexed
        // in bytes, so convert in both directions through the prefix's encoding.
        const auto byteToUnit = [&raw](uint64_t byteOff) {
            const auto n = static_cast<qsizetype>(std::min<uint64_t>(byteOff, raw.size()));
            return QString::fromUtf8(raw.data(), n).size();
        };
        const auto unitToByte = [&text](qsizetype unit) {
            return static_cast<uint64_t>(text.left(unit).toUtf8().size());
        };

        const qsizetype fromUnit = byteToUnit(from);

        // Collect the match nearest the cursor in the requested direction,
        // wrapping around the document when there is none ahead.
        auto lastMatchBefore = [&](qsizetype limit) {
            QRegularExpressionMatch best;
            qsizetype at = 0;
            while (true) {
                const auto m = re.match(text, at);
                if (!m.hasMatch() || m.capturedStart() >= limit) break;
                best = m;
                at   = m.capturedStart() + std::max<qsizetype>(1, m.capturedLength());
            }
            return best;
        };

        QRegularExpressionMatch hit;
        if (backwards) {
            hit = lastMatchBefore(fromUnit);
            if (!hit.hasMatch()) hit = lastMatchBefore(text.size());
        } else {
            hit = re.match(text, fromUnit);
            if (!hit.hasMatch()) hit = re.match(text, 0);
        }

        if (hit.hasMatch()) {
            matchStart  = unitToByte(hit.capturedStart());
            matchLength = static_cast<uint64_t>(hit.captured().toUtf8().size());
        }
    } else {
        const QByteArray needle = term.toUtf8();
        SearchEngine::Options opts;
        opts.caseSensitive = m_findBar->caseSensitive();
        opts.wrapAround    = true;

        const std::string_view sv(needle.constData(), static_cast<size_t>(needle.size()));
        matchStart = backwards
            ? SearchEngine::findBackward(*m_pieceTable, sv, from, opts)
            : SearchEngine::findForward(*m_pieceTable, sv, from, opts);
        matchLength = static_cast<uint64_t>(needle.size());
    }

    if (matchStart == SearchEngine::kNotFound) {
        m_viewport->clearMatchHighlight();
        m_findBar->setStatus(tr("No match"), true);
        m_lastMatchLength = 0;
        return false;
    }

    m_lastMatchStart  = matchStart;
    m_lastMatchLength = matchLength;
    m_viewport->setMatchHighlight(matchStart, matchLength);
    if (moveCursor) m_viewport->selectRange(matchStart, matchStart + matchLength);

    const uint64_t line = m_lineIndex ? m_lineIndex->offsetToLine(matchStart) : 0;
    m_findBar->setStatus(tr("Line %1").arg(line + 1));
    return true;
}

void MainWindow::onFindNext()
{
    // Start one byte past the current match so repeated F3 advances.
    const uint64_t from = m_viewport->hasSelection()
        ? m_viewport->selectionStart() + 1
        : m_viewport->cursorOffset();
    findFrom(from, false);
}

void MainWindow::onFindPrevious()
{
    findFrom(m_viewport->hasSelection() ? m_viewport->selectionStart()
                                        : m_viewport->cursorOffset(), true);
}

void MainWindow::onFindOptionsChanged()
{
    // Incremental search from the current match's start, so typing extends the
    // same match rather than skipping ahead.
    findFrom(m_lastMatchLength > 0 ? m_lastMatchStart : m_viewport->cursorOffset(), false);
}

void MainWindow::onReplaceCurrent()
{
    if (m_isReadOnly || !m_viewport->hasSelection() || m_lastMatchLength == 0) {
        onFindNext();
        return;
    }
    m_viewport->insertText(m_findBar->replacement());
    onFindNext();
}

void MainWindow::onReplaceAll()
{
    if (!m_pieceTable || m_isReadOnly) return;

    const QString term = m_findBar->searchTerm();
    if (term.isEmpty()) return;
    if (m_findBar->useRegex()) {
        QMessageBox::information(this, tr("Replace All"),
            tr("Replace All is available for plain-text search only."));
        return;
    }

    const QByteArray needle      = term.toUtf8();
    const QByteArray replacement = m_findBar->replacement().toUtf8();
    const std::string_view sv(needle.constData(), static_cast<size_t>(needle.size()));

    SearchEngine::Options opts;
    opts.caseSensitive = m_findBar->caseSensitive();
    opts.wrapAround    = false;

    // Collect every match first, then rewrite back-to-front so earlier offsets
    // stay valid, all inside one undo group (SRC-09).
    std::vector<uint64_t> hits;
    uint64_t at = 0;
    while (true) {
        const uint64_t hit = SearchEngine::findForward(*m_pieceTable, sv, at, opts);
        if (hit == SearchEngine::kNotFound) break;
        hits.push_back(hit);
        at = hit + needle.size();
    }
    if (hits.empty()) { m_findBar->setStatus(tr("No match"), true); return; }

    m_pieceTable->beginUndoGroup();
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        m_pieceTable->replace(*it, static_cast<uint64_t>(needle.size()),
                              std::string_view(replacement.constData(),
                                               static_cast<size_t>(replacement.size())));
    }
    m_pieceTable->endUndoGroup();

    m_lineIndex->invalidateFrom(hits.front());
    m_viewport->clearMatchHighlight();
    m_viewport->clearSelection();
    m_viewport->update();
    setDirty(true);
    updateEditActions();
    scheduleValidation();
    m_findBar->setStatus(tr("%1 replaced").arg(hits.size()));
}

void MainWindow::onFindDismissed()
{
    m_findBar->hide();
    m_viewport->clearMatchHighlight();
    m_viewport->setFocus();
}

// --- Validation ---

void MainWindow::scheduleValidation()
{
    if (m_validateTimer) m_validateTimer->start();
}

void MainWindow::onValidationTimeout()
{
    if (!m_pieceTable) return;

    // Supersede any check still running against an older revision of the
    // document, then start a fresh one.
    cancelValidation();

    m_statusValid->setText(tr("… checking"));
    m_statusValid->setStyleSheet({});
    m_statusValid->setToolTip({});

    m_validateCancel = std::make_shared<std::atomic<bool>>(false);
    auto flag        = m_validateCancel;
    const PieceTable* doc = m_pieceTable.get();

    // libxml2 streams the document, so there is no size limit to respect here;
    // it runs off the UI thread because a 2 GB check takes seconds.
    m_validateWatcher->setFuture(QtConcurrent::run([doc, flag] {
        return Validator::validate(*doc, flag.get());
    }));
}

void MainWindow::cancelValidation()
{
    if (m_validateCancel) m_validateCancel->store(true);
    m_validateCancel.reset();
}

void MainWindow::onValidationFinished()
{
    if (m_validateWatcher->isCanceled()) return;
    const ValidationResult result = m_validateWatcher->result();

    // A result for a document revision we have since moved past.
    if (result.cancelled) return;

    if (result.wellFormed) {
        m_statusValid->setText(QStringLiteral("✔ ") + tr("Well-formed"));
        m_statusValid->setStyleSheet(QStringLiteral("color: #067d17;"));
        m_statusValid->setToolTip({});
        return;
    }

    m_statusValid->setStyleSheet(QStringLiteral("color: #cc0000;"));

    const XmlDiagnostic* first = result.primary();
    if (!first) {
        m_statusValid->setText(QStringLiteral("✘ ") + tr("Not well-formed"));
        m_statusValid->setToolTip({});
        return;
    }

    m_statusValid->setText(QStringLiteral("✘ ") + tr("Line %1").arg(first->line));

    // Full diagnostic list in the tooltip, newest parser complaints first.
    QStringList lines;
    for (const auto& d : result.diagnostics) {
        lines << tr("Line %1, column %2: %3")
                     .arg(d.line)
                     .arg(d.column)
                     .arg(QString::fromStdString(d.message));
    }
    if (result.truncated)
        lines << tr("… further errors suppressed");
    m_statusValid->setToolTip(lines.join(QLatin1Char('\n')));
}

// --- External modification ---

void MainWindow::watchCurrentFile()
{
    if (!m_watcher) return;
    if (!m_watcher->files().isEmpty()) m_watcher->removePaths(m_watcher->files());
    if (!m_currentPath.isEmpty()) m_watcher->addPath(m_currentPath);
}

void MainWindow::onFileChangedOnDisk(const QString& path)
{
    if (m_ignoreNextWatchEvent) {
        m_ignoreNextWatchEvent = false;
        // QSaveFile's rename drops the watch; re-arm it.
        watchCurrentFile();
        return;
    }

    const auto choice = QMessageBox::question(
        this, tr("File changed on disk"),
        tr("%1 was modified by another program.\n\nReload it?").arg(path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (choice == QMessageBox::Yes) {
        const uint64_t line = m_viewport->cursorLine();
        setDirty(false); // the user chose to discard
        openFile(path);
        // Restore the caret once the reload has produced an index.
        QTimer::singleShot(0, this, [this, line] { gotoLine(line); });
    }
    watchCurrentFile();
}

// --- Private setup ---

void MainWindow::setupUi()
{
    resize(1280, 800);

    m_breadcrumb = new QLabel(this);
    m_breadcrumb->setWordWrap(false);
    m_breadcrumb->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_breadcrumb->setContentsMargins(6, 3, 6, 3);

    auto* central = new QWidget(this);
    auto* vbox    = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_breadcrumb);

    m_splitter = new QSplitter(Qt::Horizontal, central);

    // Left side: viewport with the find bar underneath.
    auto* left     = new QWidget(m_splitter);
    auto* leftBox  = new QVBoxLayout(left);
    leftBox->setContentsMargins(0, 0, 0, 0);
    leftBox->setSpacing(0);

    m_viewport = new ViewportRenderer(left);
    m_findBar  = new FindBar(left);
    m_findBar->hide();
    leftBox->addWidget(m_viewport, 1);
    leftBox->addWidget(m_findBar);

    m_treeView = new QTreeView(m_splitter);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->header()->setStretchLastSection(true);

    m_splitter->addWidget(left);
    m_splitter->addWidget(m_treeView);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);

    vbox->addWidget(m_splitter, 1);
    setCentralWidget(central);

    // Attribute dock
    m_attrDock  = new QDockWidget(tr("Attributes"), this);
    m_attrTable = new QTableWidget(0, 2, m_attrDock);
    m_attrTable->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    m_attrTable->horizontalHeader()->setStretchLastSection(true);
    m_attrTable->verticalHeader()->setVisible(false);
    m_attrTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attrDock->setWidget(m_attrTable);
    addDockWidget(Qt::RightDockWidgetArea, m_attrDock);
}

void MainWindow::setupMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New"),     QKeySequence::New,  this, &MainWindow::onFileNew);
    file->addAction(tr("&Open…"),   QKeySequence::Open, this, &MainWindow::onFileOpen);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    file->addAction(tr("&Save"),     QKeySequence::Save,   this, [this] { onFileSave(); });
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, [this] { onFileSaveAs(); });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::onEditUndo);
    m_redoAction = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, &MainWindow::onEditRedo);
    edit->addSeparator();
    edit->addAction(tr("Cu&t"),    QKeySequence::Cut,       m_viewport, &ViewportRenderer::cut);
    edit->addAction(tr("&Copy"),   QKeySequence::Copy,      m_viewport, &ViewportRenderer::copy);
    edit->addAction(tr("&Paste"),  QKeySequence::Paste,     m_viewport, &ViewportRenderer::paste);
    edit->addAction(tr("Select &All"), QKeySequence::SelectAll, m_viewport, &ViewportRenderer::selectAll);
    edit->addSeparator();
    edit->addAction(tr("&Find…"),           QKeySequence::Find,     this, &MainWindow::onEditFind);
    edit->addAction(tr("Find &Next"),       QKeySequence::FindNext, this, &MainWindow::onFindNext);
    edit->addAction(tr("Find &Previous"),   QKeySequence::FindPrevious, this, &MainWindow::onFindPrevious);
    edit->addAction(tr("Find && &Replace…"), QKeySequence::Replace, this, &MainWindow::onEditFindReplace);
    edit->addAction(tr("&Go to Line…"), QKeySequence(Qt::CTRL | Qt::Key_G),
                    this, &MainWindow::onEditGoToLine);

    auto* fmt = menuBar()->addMenu(tr("F&ormat"));
    fmt->addAction(tr("&Beautify"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B),
                   this, &MainWindow::onFormatBeautify);
    fmt->addAction(tr("&Minify"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
                   this, &MainWindow::onFormatMinify);

    auto* view = menuBar()->addMenu(tr("&View"));
    m_wordWrapAction = view->addAction(tr("Toggle &Word Wrap"), QKeySequence(Qt::ALT | Qt::Key_Z),
                                       this, &MainWindow::onViewToggleWordWrap);
    m_wordWrapAction->setCheckable(true);
    m_wholeDocAction = view->addAction(tr("Show &Whole Document"),
                                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W),
                                       this, &MainWindow::onShowWholeDocument);
    m_wholeDocAction->setEnabled(false);
    view->addSeparator();
    view->addAction(tr("Toggle &Tree Pane"),      {}, this, &MainWindow::onViewToggleTreePane);
    view->addAction(tr("Toggle &Attribute Pane"), {}, this, &MainWindow::onViewToggleAttrPane);
    view->addSeparator();
    m_darkAction = view->addAction(tr("&Dark Theme"), this, &MainWindow::onViewToggleDarkTheme);
    m_darkAction->setCheckable(true);
}

void MainWindow::setupStatusBar()
{
    m_statusPos      = new QLabel(tr("Ln 1, Col 1"));
    m_statusEncoding = new QLabel(QStringLiteral("UTF-8"));
    m_statusValid    = new QLabel();
    m_progress       = new QProgressBar();
    m_progress->setFixedWidth(150);
    m_progress->setVisible(false);

    statusBar()->addPermanentWidget(m_statusPos);
    statusBar()->addPermanentWidget(m_statusEncoding);
    statusBar()->addPermanentWidget(m_statusValid);
    statusBar()->addPermanentWidget(m_progress);
}

void MainWindow::connectSignals()
{
    connect(m_viewport, &ViewportRenderer::cursorMoved,    this, &MainWindow::onCursorMoved);
    connect(m_viewport, &ViewportRenderer::documentEdited, this, &MainWindow::onDocumentEdited);
    connect(m_treeView, &QTreeView::activated,            this, &MainWindow::onTreeNodeActivated);
    connect(m_treeView, &QTreeView::customContextMenuRequested,
            this, &MainWindow::onTreeContextMenu);
    connect(m_treeView, &QTreeView::clicked,              this, &MainWindow::onTreeNodeActivated);

    connect(m_findBar, &FindBar::findNext,        this, &MainWindow::onFindNext);
    connect(m_findBar, &FindBar::findPrevious,    this, &MainWindow::onFindPrevious);
    connect(m_findBar, &FindBar::replaceCurrent,  this, &MainWindow::onReplaceCurrent);
    connect(m_findBar, &FindBar::replaceAll,      this, &MainWindow::onReplaceAll);
    connect(m_findBar, &FindBar::optionsChanged,  this, &MainWindow::onFindOptionsChanged);
    connect(m_findBar, &FindBar::dismissed,       this, &MainWindow::onFindDismissed);
}

void MainWindow::updateEditActions()
{
    const bool haveDoc = m_pieceTable != nullptr;
    m_undoAction->setEnabled(haveDoc && m_pieceTable->canUndo() && !m_isReadOnly);
    m_redoAction->setEnabled(haveDoc && m_pieceTable->canRedo() && !m_isReadOnly);

    if (haveDoc && m_pieceTable->undoTruncated()) {
        statusBar()->showMessage(
            tr("Undo history trimmed to stay within the 512 MB cap."), 5000);
    }
}

void MainWindow::setDirty(bool dirty)
{
    if (m_dirty == dirty) return;
    m_dirty = dirty;
    updateWindowTitle();
}

bool MainWindow::confirmDiscardChanges()
{
    if (!m_dirty) return true;

    const auto choice = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has unsaved changes.")
            .arg(m_currentPath.isEmpty() ? tr("This document") : QFileInfo(m_currentPath).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    switch (choice) {
    case QMessageBox::Save:    return onFileSave();
    case QMessageBox::Discard: return true;
    default:                   return false;
    }
}

void MainWindow::updateRecentFiles(const QString& path)
{
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > kMaxRecentFiles) m_recentFiles.removeLast();
    rebuildRecentFileMenu();
}

void MainWindow::rebuildRecentFileMenu()
{
    m_recentMenu->clear();
    if (m_recentFiles.isEmpty()) {
        m_recentMenu->addAction(tr("(none)"))->setEnabled(false);
        return;
    }
    for (const QString& path : m_recentFiles) {
        auto* action = m_recentMenu->addAction(QFileInfo(path).fileName() + "  —  " + path);
        action->setData(path);
        connect(action, &QAction::triggered, this, &MainWindow::onFileRecentTriggered);
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear list"), this, [this] {
        m_recentFiles.clear();
        rebuildRecentFileMenu();
    });
}

void MainWindow::saveSession()
{
    QSettings s;
    s.setValue("geometry",    saveGeometry());
    s.setValue("windowState", saveState());
    s.setValue("splitter",    m_splitter->saveState());
    s.setValue("recentFiles", m_recentFiles);
    s.setValue("darkTheme",   m_darkAction->isChecked());
    s.setValue("wordWrap",    m_viewport->wordWrap());
    s.setValue("lastFile",    m_currentPath);
}

void MainWindow::restoreSession()
{
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    restoreState(s.value("windowState").toByteArray());
    m_splitter->restoreState(s.value("splitter").toByteArray());

    m_recentFiles = s.value("recentFiles").toStringList();
    rebuildRecentFileMenu();

    if (s.value("darkTheme").toBool()) {
        m_darkAction->setChecked(true);
        m_viewport->setDarkTheme(true);
    }
    if (s.value("wordWrap").toBool()) {
        m_wordWrapAction->setChecked(true);
        m_viewport->setWordWrap(true);
    }
}

void MainWindow::updateWindowTitle()
{
    const QString name = m_currentPath.isEmpty()
        ? tr("Untitled") : QFileInfo(m_currentPath).fileName();
    QString title = QStringLiteral("%1%2 — loxe").arg(name, m_dirty ? QStringLiteral("*") : QString());
    if (previewActive())                          title.prepend(tr("[parsed <%1>] ").arg(m_previewName));
    else if (m_viewport && m_viewport->hasViewRange()) title.prepend(tr("[element view] "));
    if (m_isReadOnly) title.prepend(tr("[read-only] "));
    setWindowTitle(title);
}

void MainWindow::setProgressVisible(bool visible)
{
    m_progress->setVisible(visible);
    if (visible) m_progress->setValue(0);
}
