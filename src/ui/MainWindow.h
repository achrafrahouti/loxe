#pragma once

#include <QMainWindow>
#include <QStringList>
#include <atomic>
#include <memory>

struct ValidationResult;
template <typename T> class QFutureWatcher;

class ViewportRenderer;
class VirtualTreeModel;
class AsyncLoader;
class FindBar;
class MmapBuffer;
class PieceTable;
class SparseLineIndex;
class QAction;
class QDockWidget;
class QFileSystemWatcher;
class QLabel;
class QProgressBar;
class QSplitter;
class QTableWidget;
class QTimer;
class QTreeView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);
    void newDocument();
    void gotoLine(uint64_t line);           // 0-based
    void searchFor(const QString& term);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private slots:
    // File menu
    void onFileNew();
    void onFileOpen();
    bool onFileSave();
    bool onFileSaveAs();
    void onFileRecentTriggered();

    // Edit menu
    void onEditUndo();
    void onEditRedo();
    void onEditFind();
    void onEditFindReplace();
    void onEditGoToLine();

    // Format menu
    void onFormatBeautify();
    void onFormatMinify();

    // View menu
    void onViewToggleWordWrap();
    void onViewToggleTreePane();
    void onViewToggleAttrPane();
    void onViewToggleDarkTheme();

    // AsyncLoader signals
    void onFileReady(MmapBuffer* buf, PieceTable* table, SparseLineIndex* index);
    void onIndexReady();
    void onTreeReady(VirtualTreeModel* model);
    void onLoadProgress(int percent, QString phase);
    void onLoadFailed(QString reason);

    // Sync between panes
    void onCursorMoved(uint64_t byteOffset);
    void onDocumentEdited(uint64_t offset);
    void onTreeNodeActivated(const QModelIndex& index);

    // Search
    void onFindNext();
    void onFindPrevious();
    void onReplaceCurrent();
    void onReplaceAll();
    void onFindOptionsChanged();
    void onFindDismissed();

    // Background / deferred
    void onValidationTimeout();
    void onValidationFinished();
    void onFileChangedOnDisk(const QString& path);

private:
    void setupUi();
    void setupMenus();
    void setupStatusBar();
    void connectSignals();
    void saveSession();
    void restoreSession();
    void updateWindowTitle();
    void setProgressVisible(bool visible);

    void runFormat(bool minify);
    void setDirty(bool dirty);
    // Returns false when the user cancels an unsaved-changes prompt.
    bool confirmDiscardChanges();
    bool writeDocumentTo(const QString& path, QString* error);

    void updateRecentFiles(const QString& path);
    void rebuildRecentFileMenu();
    void updateEditActions();
    void updateContextPanels(uint64_t byteOffset);
    void scheduleValidation();
    void cancelValidation();
    void watchCurrentFile();

    // Locates `term` from `from`; returns false when there is no match.
    bool findFrom(uint64_t from, bool backwards, bool moveCursor = true);

    // Owned document state
    std::unique_ptr<MmapBuffer>      m_mmapBuf;
    std::unique_ptr<PieceTable>      m_pieceTable;
    std::unique_ptr<SparseLineIndex> m_lineIndex;

    // Widgets (owned by Qt parent hierarchy)
    ViewportRenderer* m_viewport       = nullptr;
    QTreeView*        m_treeView       = nullptr;
    QSplitter*        m_splitter       = nullptr;
    QDockWidget*      m_attrDock       = nullptr;
    QTableWidget*     m_attrTable      = nullptr;
    QLabel*           m_breadcrumb     = nullptr;
    QLabel*           m_statusPos      = nullptr;
    QLabel*           m_statusEncoding = nullptr;
    QLabel*           m_statusValid    = nullptr;
    QProgressBar*     m_progress       = nullptr;
    FindBar*          m_findBar        = nullptr;

    QAction* m_undoAction     = nullptr;
    QAction* m_redoAction     = nullptr;
    QAction* m_wordWrapAction = nullptr;
    QAction* m_darkAction     = nullptr;
    QMenu*   m_recentMenu     = nullptr;

    AsyncLoader*        m_loader        = nullptr;
    QTimer*             m_validateTimer = nullptr;
    QFileSystemWatcher* m_watcher       = nullptr;

    // Well-formedness runs on a worker thread; the flag lets a superseded run
    // abandon its parse as soon as the document changes underneath it. Shared
    // so the in-flight lambda keeps it alive after we have dropped our copy.
    QFutureWatcher<ValidationResult>*  m_validateWatcher = nullptr;
    std::shared_ptr<std::atomic<bool>> m_validateCancel;

    QString     m_currentPath;
    QStringList m_recentFiles;
    bool        m_isReadOnly = false;
    bool        m_dirty      = false;
    bool        m_ignoreNextWatchEvent = false;
    uint64_t    m_lastMatchStart  = 0;
    uint64_t    m_lastMatchLength = 0;
};
