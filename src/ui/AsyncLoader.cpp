#include "AsyncLoader.h"
#include "VirtualTreeModel.h"
#include "../engine/MmapBuffer.h"
#include "../engine/PieceTable.h"
#include "../engine/SparseLineIndex.h"
#include "Markup.h"

#include <QMetaObject>

AsyncLoader::AsyncLoader(QObject* parent) : QObject(parent)
{
    connect(&m_thread, &QThread::started, this, &AsyncLoader::run,
            Qt::DirectConnection);
}

AsyncLoader::~AsyncLoader()
{
    cancel();
    m_thread.wait();
}

void AsyncLoader::load(const QString& path)
{
    if (m_thread.isRunning()) { cancel(); m_thread.wait(); }
    m_path   = path;
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
    // --- Phase 1: mmap ---
    emit loadProgress(0, "Opening file…");

    auto* buf = new MmapBuffer();
    if (!buf->open(m_path.toUtf8().constData())) {
        emit loadFailed(QString("Cannot open: %1").arg(m_path));
        delete buf;
        return;
    }
    buf->adviseSequential();

    auto* table = new PieceTable(buf);
    emit fileReady(buf, table); // main thread takes ownership

    if (m_cancel.load()) return;

    // --- Phase 2: SparseLineIndex ---
    emit loadProgress(5, "Building line index…");

    auto* index = new SparseLineIndex();
    if (!index->build(*buf, m_cancel)) {
        delete index;
        return;
    }
    emit indexReady(index);

    if (m_cancel.load()) return;

    // --- Phase 3: VirtualTreeModel level-1 parse ---
    emit loadProgress(80, "Parsing tree structure…");

    auto* model = new VirtualTreeModel(m_path);

    CMarkup markup(CMarkup::MDF_READFILE);
    if (markup.Load(m_path.toStdString()) && markup.FindElem()) {
        markup.IntoElem();  // enter XML root element

        std::vector<TreeNode> roots;
        int sibIdx = 0;
        while (!m_cancel.load() && markup.FindElem()) {
            TreeNode node;
            node.tagName     = QString::fromStdString(markup.GetTagName());
            node.depth       = 1;
            node.parentIndex = -1;
            node.navPath     = {sibIdx};

            std::string aName, aVal;
            if (markup.GetNthAttrib(1, aName, aVal) && !aName.empty())
                node.attrSummary = QString("[@%1=\"%2\"]")
                    .arg(QString::fromStdString(aName))
                    .arg(QString::fromStdString(aVal).left(30));

            const std::string preview = markup.GetData();
            if (!preview.empty())
                node.textPreview = QString::fromUtf8(preview.c_str()).simplified().left(60);

            markup.IntoElem();
            node.hasChildren = markup.FindElem();
            markup.OutOfElem();

            roots.push_back(std::move(node));
            ++sibIdx;
        }

        if (!roots.empty())
            model->appendRootNodes(std::move(roots));
    }

    emit treeReady(model);
    emit loadProgress(100, "Done");
}
