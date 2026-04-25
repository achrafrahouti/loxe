#pragma once

#include <QMainWindow>
#include <memory>

class ViewportRenderer;
class VirtualTreeModel;
class AsyncLoader;
class MmapBuffer;
class PieceTable;
class SparseLineIndex;
class QTreeView;
class QSplitter;
class QDockWidget;
class QTableWidget;
class QLabel;
class QProgressBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private slots:
    // File menu
    void onFileNew();
    void onFileOpen();
    void onFileSave();
    void onFileSaveAs();
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

    // AsyncLoader signals
    void onFileReady(MmapBuffer* buf, PieceTable* table);
    void onIndexReady(SparseLineIndex* index);
    void onTreeReady(VirtualTreeModel* model);
    void onLoadProgress(int percent, QString phase);
    void onLoadFailed(QString reason);

    // Sync between panes
    void onCursorMoved(uint64_t byteOffset);
    void onTreeNodeActivated(const QModelIndex& index);

private:
    void setupUi();
    void setupMenus();
    void setupStatusBar();
    void connectSignals();
    void saveSession();
    void restoreSession();
    void updateWindowTitle();
    void setProgressVisible(bool visible);

    // Owned document state
    std::unique_ptr<MmapBuffer>      m_mmapBuf;
    std::unique_ptr<PieceTable>      m_pieceTable;
    std::unique_ptr<SparseLineIndex> m_lineIndex;

    // Widgets (owned by Qt parent hierarchy)
    ViewportRenderer* m_viewport        = nullptr;
    QTreeView*        m_treeView        = nullptr;
    QSplitter*        m_splitter        = nullptr;
    QDockWidget*      m_attrDock        = nullptr;
    QTableWidget*     m_attrTable       = nullptr;
    QLabel*           m_breadcrumb      = nullptr;
    QLabel*           m_statusPos       = nullptr;
    QLabel*           m_statusEncoding  = nullptr;
    QLabel*           m_statusValid     = nullptr;
    QProgressBar*     m_progress        = nullptr;

    AsyncLoader* m_loader      = nullptr;
    QString      m_currentPath;
    bool         m_isReadOnly  = false;
};
