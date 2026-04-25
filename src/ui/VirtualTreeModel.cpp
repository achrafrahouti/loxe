#include "VirtualTreeModel.h"
#include "../engine/MmapBuffer.h"

#include <QFont>

VirtualTreeModel::VirtualTreeModel(MmapBuffer* buf, QObject* parent)
    : QAbstractItemModel(parent), m_buf(buf)
{}

void VirtualTreeModel::appendRootNodes(std::vector<TreeNode> nodes)
{
    if (nodes.empty()) return;
    beginInsertRows({}, static_cast<int>(m_nodes.size()),
                    static_cast<int>(m_nodes.size() + nodes.size() - 1));
    for (auto& n : nodes) m_nodes.push_back(std::move(n));
    endInsertRows();
}

QModelIndex VirtualTreeModel::index(int row, int column,
                                     const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent)) return {};

    if (!parent.isValid()) {
        // Root items: find the row-th node with parentIndex == -1
        int count = 0;
        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
            if (m_nodes[i].parentIndex == -1) {
                if (count == row) return createIndex(row, column, i);
                ++count;
            }
        }
        return {};
    }

    const int parentNodeIdx = nodeIndexOf(parent);
    // TODO: walk firstChild/nextSibling chain to find the row-th child
    (void)parentNodeIdx;
    return {};
}

QModelIndex VirtualTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    const int nodeIdx    = nodeIndexOf(child);
    const int parentIdx  = m_nodes[nodeIdx].parentIndex;
    if (parentIdx < 0) return {};
    // TODO: compute the row of parentIdx among its own siblings
    return createIndex(0, 0, parentIdx);
}

int VirtualTreeModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) {
        int count = 0;
        for (const auto& n : m_nodes) if (n.parentIndex == -1) ++count;
        return count;
    }
    const int nodeIdx = nodeIndexOf(parent);
    if (!m_nodes[nodeIdx].isLoaded) return 0;
    int count = 0;
    for (const auto& n : m_nodes) if (n.parentIndex == nodeIdx) ++count;
    return count;
}

int VirtualTreeModel::columnCount(const QModelIndex&) const { return 3; }

QVariant VirtualTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const auto& node = m_nodes[nodeIndexOf(index)];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return node.tagName;
        case 1: return {}; // TODO: attribute summary
        case 2: return {}; // TODO: text preview (≤60 chars)
        }
    }
    return {};
}

QVariant VirtualTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return "Element";
    case 1: return "Attributes";
    case 2: return "Text";
    }
    return {};
}

bool VirtualTreeModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid()) return !m_nodes.empty();
    return m_nodes[nodeIndexOf(parent)].hasChildren;
}

bool VirtualTreeModel::canFetchMore(const QModelIndex& parent) const
{
    if (!parent.isValid()) return false;
    const auto& node = m_nodes[nodeIndexOf(parent)];
    return node.hasChildren && !node.isLoaded;
}

void VirtualTreeModel::fetchMore(const QModelIndex& parent)
{
    if (!canFetchMore(parent)) return;
    loadChildren(nodeIndexOf(parent));
}

uint64_t VirtualTreeModel::byteOffsetFor(const QModelIndex& index) const
{
    if (!index.isValid()) return 0;
    return m_nodes[nodeIndexOf(index)].byteOffset;
}

void VirtualTreeModel::loadChildren(int nodeIndex)
{
    // TODO: seek CMarkup to m_nodes[nodeIndex].byteOffset (MDF_READFILE),
    // parse forward until depth returns to parent depth,
    // emit child TreeNodes, call beginInsertRows / endInsertRows.
    m_nodes[nodeIndex].isLoaded = true;
}

int VirtualTreeModel::nodeIndexOf(const QModelIndex& idx) const
{
    return static_cast<int>(idx.internalId());
}
