#include "MainWindow.h"
#include "AsyncLoader.h"
#include "ViewportRenderer.h"
#include "VirtualTreeModel.h"
#include "../engine/MmapBuffer.h"
#include "../engine/PieceTable.h"
#include "../engine/SparseLineIndex.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTreeView>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setupUi();
    setupMenus();
    setupStatusBar();
    connectSignals();
    restoreSession();
    setAcceptDrops(true);
}

MainWindow::~MainWindow() = default;

void MainWindow::openFile(const QString& path)
{
    if (!m_loader) {
        m_loader = new AsyncLoader(this);
        connect(m_loader, &AsyncLoader::fileReady,    this, &MainWindow::onFileReady);
        connect(m_loader, &AsyncLoader::indexReady,   this, &MainWindow::onIndexReady);
        connect(m_loader, &AsyncLoader::treeReady,    this, &MainWindow::onTreeReady);
        connect(m_loader, &AsyncLoader::loadProgress, this, &MainWindow::onLoadProgress);
        connect(m_loader, &AsyncLoader::loadFailed,   this, &MainWindow::onLoadFailed);
    }
    m_currentPath = path;
    updateWindowTitle();
    setProgressVisible(true);
    m_loader->load(path);
}

// --- Protected events ---

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveSession();
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

// --- File menu slots ---

void MainWindow::onFileNew()
{
    // TODO: create empty PieceTable, clear viewport and tree
}

void MainWindow::onFileOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open XML File", {}, "XML Files (*.xml *.xhtml *.html);;All Files (*)");
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::onFileSave()
{
    if (m_currentPath.isEmpty()) { onFileSaveAs(); return; }
    // TODO: atomic save (write temp + rename)
}

void MainWindow::onFileSaveAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, "Save As", m_currentPath, "XML Files (*.xml);;All Files (*)");
    if (!path.isEmpty()) { m_currentPath = path; onFileSave(); }
}

void MainWindow::onFileRecentTriggered()
{
    // TODO: get path from QAction::data() and openFile()
}

// --- Edit menu slots ---

void MainWindow::onEditUndo()   { if (m_pieceTable) m_pieceTable->undo(); }
void MainWindow::onEditRedo()   { if (m_pieceTable) m_pieceTable->redo(); }
void MainWindow::onEditFind()   { /* TODO: show inline search bar */ }
void MainWindow::onEditFindReplace() { /* TODO: show find/replace dialog */ }
void MainWindow::onEditGoToLine()    { /* TODO: show go-to-line dialog */ }

// --- Format menu slots ---

void MainWindow::onFormatBeautify() { /* TODO: run FormatEngine in background */ }
void MainWindow::onFormatMinify()   { /* TODO: run FormatEngine(Minify) */ }

// --- View menu slots ---

void MainWindow::onViewToggleWordWrap()  { m_viewport->setWordWrap(!m_viewport->wordWrap()); }
void MainWindow::onViewToggleTreePane()  { m_treeView->setVisible(!m_treeView->isVisible()); }
void MainWindow::onViewToggleAttrPane()  { m_attrDock->setVisible(!m_attrDock->isVisible()); }

// --- AsyncLoader slots ---

void MainWindow::onFileReady(MmapBuffer* buf, PieceTable* table)
{
    m_mmapBuf.reset(buf);
    m_pieceTable.reset(table);
    m_viewport->setDocument(table, nullptr);
}

void MainWindow::onIndexReady(SparseLineIndex* index)
{
    m_lineIndex.reset(index);
    m_viewport->setDocument(m_pieceTable.get(), index);
}

void MainWindow::onTreeReady(VirtualTreeModel* model)
{
    m_treeView->setModel(model);
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
    QMessageBox::critical(this, "Open failed", reason);
}

// --- Sync ---

void MainWindow::onCursorMoved(uint64_t byteOffset)
{
    if (!m_lineIndex) return;
    const uint64_t line = m_lineIndex->offsetToLine(byteOffset);
    const uint64_t col  = byteOffset - m_lineIndex->lineToOffset(line);
    m_statusPos->setText(QString("Ln %1, Col %2").arg(line + 1).arg(col + 1));
    // TODO: update breadcrumb and highlight matching tree node
    (void)byteOffset;
}

void MainWindow::onTreeNodeActivated(const QModelIndex& index)
{
    const auto* model = qobject_cast<const VirtualTreeModel*>(m_treeView->model());
    if (!model) return;
    const uint64_t offset = model->byteOffsetFor(index);
    m_viewport->setCursorOffset(offset);
}

// --- Private setup ---

void MainWindow::setupUi()
{
    resize(1280, 800);

    m_breadcrumb = new QLabel(this);
    m_breadcrumb->setWordWrap(false);

    auto* central = new QWidget(this);
    auto* vbox    = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_breadcrumb);

    m_splitter  = new QSplitter(Qt::Horizontal, central);
    m_viewport  = new ViewportRenderer(m_splitter);
    m_treeView  = new QTreeView(m_splitter);
    m_splitter->addWidget(m_viewport);
    m_splitter->addWidget(m_treeView);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 1);

    vbox->addWidget(m_splitter);
    setCentralWidget(central);

    // Attribute dock
    m_attrDock  = new QDockWidget("Attributes", this);
    m_attrTable = new QTableWidget(0, 2, m_attrDock);
    m_attrTable->setHorizontalHeaderLabels({"Name", "Value"});
    m_attrDock->setWidget(m_attrTable);
    addDockWidget(Qt::RightDockWidgetArea, m_attrDock);
}

void MainWindow::setupMenus()
{
    auto* file = menuBar()->addMenu("&File");
    file->addAction("&New",      QKeySequence::New,    this, &MainWindow::onFileNew);
    file->addAction("&Open…",    QKeySequence::Open,   this, &MainWindow::onFileOpen);
    file->addAction("&Save",     QKeySequence::Save,   this, &MainWindow::onFileSave);
    file->addAction("Save &As…", QKeySequence::SaveAs, this, &MainWindow::onFileSaveAs);
    file->addSeparator();
    file->addAction("E&xit",     QKeySequence::Quit,   qApp, &QApplication::quit);

    auto* edit = menuBar()->addMenu("&Edit");
    edit->addAction("&Undo",             QKeySequence::Undo,    this, &MainWindow::onEditUndo);
    edit->addAction("&Redo",             QKeySequence::Redo,    this, &MainWindow::onEditRedo);
    edit->addSeparator();
    edit->addAction("&Find…",            QKeySequence::Find,    this, &MainWindow::onEditFind);
    edit->addAction("Find && Replace…",  QKeySequence::Replace, this, &MainWindow::onEditFindReplace);
    edit->addAction("&Go to Line…",      QKeySequence(Qt::CTRL | Qt::Key_G),
                    this, &MainWindow::onEditGoToLine);

    auto* fmt = menuBar()->addMenu("F&ormat");
    fmt->addAction("&Beautify", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B),
                   this, &MainWindow::onFormatBeautify);
    fmt->addAction("&Minify",   {}, this, &MainWindow::onFormatMinify);

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Toggle &Word Wrap", QKeySequence(Qt::ALT | Qt::Key_Z),
                    this, &MainWindow::onViewToggleWordWrap);
    view->addAction("Toggle &Tree Pane",      {}, this, &MainWindow::onViewToggleTreePane);
    view->addAction("Toggle &Attribute Pane", {}, this, &MainWindow::onViewToggleAttrPane);
}

void MainWindow::setupStatusBar()
{
    m_statusPos      = new QLabel("Ln 1, Col 1");
    m_statusEncoding = new QLabel("UTF-8");
    m_statusValid    = new QLabel("✔ Well-formed");
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
    connect(m_viewport, &ViewportRenderer::cursorMoved,
            this, &MainWindow::onCursorMoved);
    connect(m_treeView, &QTreeView::activated,
            this, &MainWindow::onTreeNodeActivated);
}

void MainWindow::saveSession()
{
    QSettings s;
    s.setValue("geometry",     saveGeometry());
    s.setValue("splitter",     m_splitter->saveState());
    s.setValue("recentFiles",  QStringList()); // TODO: maintain recent files list
}

void MainWindow::restoreSession()
{
    QSettings s;
    restoreGeometry(s.value("geometry").toByteArray());
    m_splitter->restoreState(s.value("splitter").toByteArray());
}

void MainWindow::updateWindowTitle()
{
    setWindowTitle(m_currentPath.isEmpty()
        ? "loxe"
        : QString("%1 — loxe").arg(QFileInfo(m_currentPath).fileName()));
}

void MainWindow::setProgressVisible(bool visible)
{
    m_progress->setVisible(visible);
    if (visible) m_progress->setValue(0);
}
