#pragma once

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

class MmapBuffer;
class PieceTable;
class SparseLineIndex;
class VirtualTreeModel;

// Coordinates three sequential loading phases on a dedicated QThread:
//   Phase 1 — mmap() + PieceTable + an index attached but not yet scanned,
//             so the viewport can paint immediately: emits fileReady()
//   Phase 2 — full SparseLineIndex scan:            emits indexReady()
//   Phase 3 — VirtualTreeModel level-1 parse:       emits treeReady()
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
    // Phase 1 complete — the viewport can display right away. The index is
    // attached to the table and extends its checkpoints lazily on lookup, so
    // it is usable before the phase 2 scan finishes.
    void fileReady(MmapBuffer* buf, PieceTable* table, SparseLineIndex* index);
    // Progress during phases 2 and 3.
    void loadProgress(int percent, QString phase);
    // Phase 2 complete — scroll bar range and line count are now exact.
    void indexReady();
    // Phase 3 complete — tree view is populated to level 1.
    void treeReady(VirtualTreeModel* model);
    void loadFailed(QString reason);

private:
    void run();

    QThread           m_thread;
    QString           m_path;
    std::atomic<bool> m_cancel{false};

    // Owned by the main thread once fileReady() has been delivered; the worker
    // keeps raw pointers only for the duration of phases 2 and 3.
    MmapBuffer*      m_buf   = nullptr;
    PieceTable*      m_table = nullptr;
    SparseLineIndex* m_index = nullptr;
};
