#pragma once

#include <QObject>
#include <QThread>
#include <atomic>

class MmapBuffer;
class PieceTable;
class SparseLineIndex;
class VirtualTreeModel;

// Coordinates three sequential loading phases on a dedicated QThread:
//   Phase 1 — mmap():           emits fileReady()
//   Phase 2 — SparseLineIndex:  emits indexReady()
//   Phase 3 — VirtualTreeModel level-1 parse: emits treeReady()
//
// Ownership of produced objects is transferred to the main thread via signals.
class AsyncLoader : public QObject {
    Q_OBJECT
public:
    explicit AsyncLoader(QObject* parent = nullptr);
    ~AsyncLoader() override;

    void load(const QString& path);
    void cancel();
    bool isRunning() const;

signals:
    // Phase 1 complete — viewport can start displaying (index still building)
    void fileReady(MmapBuffer* buf, PieceTable* table);
    // Progress during phases 2 and 3
    void loadProgress(int percent, QString phase);
    // Phase 2 complete — scroll bar range is now accurate
    void indexReady(SparseLineIndex* index);
    // Phase 3 complete — tree view is populated to level 1
    void treeReady(VirtualTreeModel* model);
    void loadFailed(QString reason);

private:
    void run();

    QThread           m_thread;
    QString           m_path;
    std::atomic<bool> m_cancel{false};
};
