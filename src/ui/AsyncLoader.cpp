#include "AsyncLoader.h"
#include "VirtualTreeModel.h"
#include "../engine/MmapBuffer.h"
#include "../engine/PieceTable.h"
#include "../engine/SparseLineIndex.h"
#include <QCoreApplication>

AsyncLoader::AsyncLoader(QObject* parent) : QObject(parent)
{
    connect(&m_thread, &QThread::started, this, &AsyncLoader::run,
            Qt::DirectConnection);
}

AsyncLoader::~AsyncLoader()
{
    cancel();
    m_thread.quit();
    m_thread.wait();
}

void AsyncLoader::load(const QString& path)
{
    if (m_thread.isRunning()) {
        cancel();
        m_thread.quit();
        m_thread.wait();
    }
    m_path = path;
    m_cancel.store(false);
    m_thread.start(); // run() executes in m_thread via Qt::DirectConnection
}

void AsyncLoader::cancel()
{
    m_cancel.store(true);
}

bool AsyncLoader::isRunning() const
{
    return m_thread.isRunning();
}

void AsyncLoader::run()
{
    // --- Phase 1: mmap, document, attached index ---
    emit loadProgress(0, QStringLiteral("Opening file…"));

    auto* buf = new MmapBuffer();
    if (!buf->open(m_path.toUtf8().constData())) {
        delete buf;
        emit loadFailed(QStringLiteral("Cannot open: %1").arg(m_path));
        return;
    }
    buf->adviseSequential();

    auto* table = new PieceTable(buf);
    auto* index = new SparseLineIndex();

    // attach() makes lookups work immediately by extending checkpoints on
    // demand, so the first screenful paints without waiting for the full scan.
    index->attach(*table);

    m_buf   = buf;
    m_table = table;
    m_index = index;
    emit fileReady(buf, table, index); // main thread takes ownership

    if (m_cancel.load()) return;

    // --- Phase 2: full SparseLineIndex scan ---
    emit loadProgress(5, QStringLiteral("Building line index…"));

    if (!index->build(*table, m_cancel, [this](int pct) {
            // Phases 2 and 3 share the bar: 5–75 % is the index scan.
            emit loadProgress(5 + pct * 70 / 100, QStringLiteral("Building line index…"));
        })) {
        return; // cancelled; the main thread owns and will free the objects
    }
    emit indexReady();

    if (m_cancel.load()) return;

    // --- Phase 3: VirtualTreeModel document element ---
    emit loadProgress(80, QStringLiteral("Parsing tree structure…"));

    // The tree reads the live PieceTable rather than the file on disk, so it
    // stays consistent with unsaved edits. scanRoots() enumerates level 1 here
    // on the worker thread — that is one pass over the document, which must not
    // land on the UI thread when the view first expands the root.
    auto* model = new VirtualTreeModel(table);
    auto  roots = VirtualTreeModel::scanRoots(*table, &m_cancel);
    if (!m_cancel.load() && !roots.empty())
        model->setInitialNodes(std::move(roots));

    if (m_cancel.load()) { delete model; return; }

    // The model was constructed on this worker thread but will be driven by a
    // view on the main thread. Hand over its thread affinity before publishing
    // it — moveToThread() is only legal from the owning thread, which is here.
    model->moveToThread(QCoreApplication::instance()->thread());

    emit treeReady(model);
    emit loadProgress(100, QStringLiteral("Ready"));
}
