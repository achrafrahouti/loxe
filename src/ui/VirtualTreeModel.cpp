#include "VirtualTreeModel.h"
#include "Markup.h"

#include <QColor>

// ── CMarkup helpers ──────────────────────────────────────────────────────────

// Navigate a fresh streaming CMarkup to the element described by path (sequence
// of 0-based sibling indices from just inside the XML root element).
// Returns true with markup positioned inside the target element.
static bool navigateTo(CMarkup& markup, const std::vector<int>& path)
{
    if (!markup.FindElem()) return false;   // XML root element
    markup.IntoElem();                       // enter root
    for (int step : path) {
        for (int j = 0; j <= step; ++j)
            if (!markup.FindElem()) return false;
        markup.IntoElem();
    }
    return true;
}

// Return "[@name="value"]" for the first attribute of the current element,
// or an empty string if none.
static QString firstAttrSummary(CMarkup& markup)
{
    std::string name, val;
    if (!markup.GetNthAttrib(1, name, val) || name.empty()) return {};
    return QString("[@%1=\"%2\"]")
        .arg(QString::fromStdString(name))
        .arg(QString::fromStdString(val).left(30));
}

// ── VirtualTreeModel ─────────────────────────────────────────────────────────

VirtualTreeModel::VirtualTreeModel(const QString& filePath, QObject* parent)
    : QAbstractItemModel(parent), m_filePath(filePath)
{}

void VirtualTreeModel::appendRootNodes(std::vector<TreeNode> nodes)
{
    if (nodes.empty()) return;
    beginInsertRows({}, static_cast<int>(m_nodes.size()),
                    static_cast<int>(m_nodes.size() + nodes.size() - 1));
    for (auto& n : nodes) m_nodes.push_back(std::move(n));
    endInsertRows();
}

// ── QAbstractItemModel ───────────────────────────────────────────────────────

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

    // Child items: walk firstChild → nextSibling chain
    const int pni      = nodeIndexOf(parent);
    int       childIdx = m_nodes[pni].firstChild;
    for (int i = 0; i < row; ++i) {
        if (childIdx == -1) return {};
        childIdx = m_nodes[childIdx].nextSibling;
    }
    if (childIdx == -1) return {};
    return createIndex(row, column, childIdx);
}

QModelIndex VirtualTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    const int nodeIdx   = nodeIndexOf(child);
    const int parentIdx = m_nodes[nodeIdx].parentIndex;
    if (parentIdx < 0) return {};
    return createIndex(rowOfNode(parentIdx), 0, parentIdx);
}

int VirtualTreeModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) {
        int count = 0;
        for (const auto& n : m_nodes)
            if (n.parentIndex == -1) ++count;
        return count;
    }
    const int nodeIdx = nodeIndexOf(parent);
    if (!m_nodes[nodeIdx].isLoaded) return 0;
    int count = 0;
    int child = m_nodes[nodeIdx].firstChild;
    while (child != -1) { ++count; child = m_nodes[child].nextSibling; }
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
        case 1: return node.attrSummary;
        case 2: return node.textPreview;
        }
    }
    if (role == Qt::ForegroundRole && index.column() == 0)
        return QColor(0x00, 0x55, 0xAA);
    return {};
}

QVariant VirtualTreeModel::headerData(int section, Qt::Orientation orientation,
                                       int role) const
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

// ── Private ──────────────────────────────────────────────────────────────────

void VirtualTreeModel::loadChildren(int nodeIndex)
{
    CMarkup markup(CMarkup::MDF_READFILE);
    if (!markup.Load(m_filePath.toStdString())) {
        m_nodes[nodeIndex].isLoaded = true;
        return;
    }

    if (!navigateTo(markup, m_nodes[nodeIndex].navPath)) {
        m_nodes[nodeIndex].isLoaded = true;
        return;
    }

    // Collect children at this level
    std::vector<TreeNode> batch;
    int sibIdx = 0;
    while (markup.FindElem()) {
        TreeNode child;
        child.tagName     = QString::fromStdString(markup.GetTagName());
        child.attrSummary = firstAttrSummary(markup);

        const std::string preview = markup.GetData();
        if (!preview.empty())
            child.textPreview = QString::fromUtf8(preview.c_str()).simplified().left(60);

        child.depth       = m_nodes[nodeIndex].depth + 1;
        child.parentIndex = nodeIndex;
        child.navPath     = m_nodes[nodeIndex].navPath;
        child.navPath.push_back(sibIdx);

        // Peek inside to determine hasChildren, then step back out
        markup.IntoElem();
        child.hasChildren = markup.FindElem();
        markup.OutOfElem();

        batch.push_back(std::move(child));
        ++sibIdx;
    }

    if (batch.empty()) {
        m_nodes[nodeIndex].isLoaded = true;
        return;
    }

    const QModelIndex parentQIdx = createIndex(rowOfNode(nodeIndex), 0, nodeIndex);
    beginInsertRows(parentQIdx, 0, static_cast<int>(batch.size()) - 1);

    const int base = static_cast<int>(m_nodes.size());
    for (int i = 0; i < static_cast<int>(batch.size()); ++i) {
        m_nodes.push_back(std::move(batch[i]));
        if (i == 0)
            m_nodes[nodeIndex].firstChild = base;
        else
            m_nodes[base + i - 1].nextSibling = base + i;
    }
    m_nodes[nodeIndex].isLoaded = true;

    endInsertRows();
}

int VirtualTreeModel::nodeIndexOf(const QModelIndex& idx) const
{
    return static_cast<int>(idx.internalId());
}

int VirtualTreeModel::rowOfNode(int nodeIdx) const
{
    const int parentIdx = m_nodes[nodeIdx].parentIndex;
    int row = 0;
    if (parentIdx < 0) {
        for (int i = 0; i < nodeIdx; ++i)
            if (m_nodes[i].parentIndex == -1) ++row;
    } else {
        int sib = m_nodes[parentIdx].firstChild;
        while (sib != -1 && sib != nodeIdx) {
            ++row;
            sib = m_nodes[sib].nextSibling;
        }
    }
    return row;
}
