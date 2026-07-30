#include "VirtualTreeModel.h"
#include "../engine/PieceTable.h"
#include "../engine/XmlScanner.h"

#include <QColor>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kTextPreviewChars = 60;
constexpr int kAttrValueChars   = 30;

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString attrSummaryOf(std::string_view rawTag)
{
    std::string_view name, value;
    if (!XmlScanner::firstAttribute(rawTag, &name, &value) || name.empty()) return {};
    return QStringLiteral("[@%1=\"%2\"]").arg(qs(name), qs(value).left(kAttrValueChars));
}

bool isBlank(std::string_view s)
{
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    return true;
}

// Result of streaming one element's byte range.
struct ChildScan {
    std::vector<TreeNode> children;
    uint64_t              parentEnd = 0;  // just past the parent's end tag
    QString               parentText;     // parent's own text content
    bool                  truncated = false;
};

// Streams the element starting at `startOffset` and collects its *direct*
// children, together with each child's hasChildren flag, end offset and text
// preview — all from a single pass over that element's subtree.
ChildScan collectChildren(const PieceTable& doc, uint64_t startOffset, int parentDepth,
                          int parentIndex, int maxChildren, uint64_t maxScanBytes,
                          const std::atomic<bool>* cancelled)
{
    ChildScan result;

    // relDepth is the nesting level relative to the parent element:
    // 0 means "a direct child", -1 means the parent's start tag is still pending.
    int relDepth  = -1;
    int lastChild = -1;

    const uint64_t scanLimit = (doc.length() - startOffset > maxScanBytes)
        ? startOffset + maxScanBytes : doc.length();

    XmlScanner::scan(doc, startOffset, scanLimit, [&](const XmlNode& n) {
        if (relDepth < 0) {
            if (n.kind == XmlNode::Kind::StartTag) { relDepth = 0; return true; }
            if (n.kind == XmlNode::Kind::EmptyTag) {
                result.parentEnd = n.offset + n.raw.size();
                return false; // no children at all
            }
            return true; // skip declaration / comments before the element
        }

        switch (n.kind) {
        case XmlNode::Kind::StartTag:
        case XmlNode::Kind::EmptyTag:
            if (relDepth == 0) {
                if (static_cast<int>(result.children.size()) >= maxChildren) {
                    result.truncated = true;
                    return false;
                }
                TreeNode child;
                child.byteOffset  = n.offset;
                child.depth       = parentDepth + 1;
                child.tagName     = qs(n.name);
                child.attrSummary = attrSummaryOf(n.raw);
                child.parentIndex = parentIndex;
                if (n.kind == XmlNode::Kind::EmptyTag)
                    child.endOffset = n.offset + n.raw.size();
                result.children.push_back(std::move(child));
                lastChild = static_cast<int>(result.children.size()) - 1;
            } else if (relDepth == 1 && lastChild >= 0) {
                // A nested element inside the most recent direct child.
                result.children[static_cast<size_t>(lastChild)].hasChildren = true;
            }
            if (n.kind == XmlNode::Kind::StartTag) ++relDepth;
            break;

        case XmlNode::Kind::EndTag:
            if (relDepth == 0) {
                result.parentEnd = n.offset + n.raw.size();
                return false; // the parent's own end tag: subtree complete
            }
            --relDepth;
            // Back at depth 0 means we just closed the most recent direct child.
            if (relDepth == 0 && lastChild >= 0)
                result.children[static_cast<size_t>(lastChild)].endOffset =
                    n.offset + n.raw.size();
            break;

        case XmlNode::Kind::Text:
            if (isBlank(n.raw)) break;
            if (relDepth == 0) {
                if (result.parentText.size() < kTextPreviewChars)
                    result.parentText += qs(n.raw).simplified();
            } else if (relDepth == 1 && lastChild >= 0) {
                TreeNode& child = result.children[static_cast<size_t>(lastChild)];
                if (child.textPreview.size() < kTextPreviewChars)
                    child.textPreview += qs(n.raw).simplified();
            }
            break;

        case XmlNode::Kind::Cdata:
        case XmlNode::Kind::Comment:
        case XmlNode::Kind::ProcessingInstruction:
        case XmlNode::Kind::Doctype:
            break;
        }
        return true;
    }, cancelled);

    // parentEnd stays 0 when the parent's end tag was never reached, i.e. the
    // byte budget cut the traversal short.
    if (result.parentEnd == 0 && scanLimit < doc.length())
        result.truncated = true;

    for (auto& child : result.children)
        child.textPreview = child.textPreview.left(kTextPreviewChars);
    result.parentText = result.parentText.left(kTextPreviewChars);
    return result;
}

} // namespace

VirtualTreeModel::VirtualTreeModel(const PieceTable* doc, QObject* parent)
    : QAbstractItemModel(parent), m_doc(doc)
{}

// ── Root scan ────────────────────────────────────────────────────────────────

std::vector<TreeNode> VirtualTreeModel::scanRoots(const PieceTable& doc,
                                                 const std::atomic<bool>* cancelled)
{
    std::vector<TreeNode> nodes;

    // Locate the document element, skipping the declaration, comments and DOCTYPE.
    TreeNode root;
    bool     found      = false;
    bool     selfClosed = false;
    XmlScanner::scanAll(doc, [&](const XmlNode& n) {
        if (n.kind != XmlNode::Kind::StartTag && n.kind != XmlNode::Kind::EmptyTag)
            return true;
        root.byteOffset  = n.offset;
        root.depth       = 0;
        root.tagName     = qs(n.name);
        root.attrSummary = attrSummaryOf(n.raw);
        root.parentIndex = -1;
        selfClosed       = (n.kind == XmlNode::Kind::EmptyTag);
        found            = true;
        return false;
    }, cancelled);

    if (!found) return nodes;

    if (selfClosed) {
        root.endOffset = root.byteOffset;
        root.isLoaded  = true;
        nodes.push_back(std::move(root));
        return nodes;
    }

    // Enumerate level 1 up front, on this (worker) thread.
    ChildScan scan = collectChildren(doc, root.byteOffset, /*parentDepth=*/0,
                                     /*parentIndex=*/0, kMaxChildrenPerNode,
                                     kMaxScanBytes, cancelled);

    root.endOffset   = scan.parentEnd;
    root.textPreview = scan.parentText;
    root.truncated   = scan.truncated;
    root.hasChildren = !scan.children.empty();
    root.isLoaded    = true;
    nodes.push_back(std::move(root));

    // Append the children and wire the sibling chain.
    const int base = 1;
    for (size_t i = 0; i < scan.children.size(); ++i) {
        nodes.push_back(std::move(scan.children[i]));
        if (i == 0) nodes[0].firstChild = base;
        else        nodes[base + static_cast<int>(i) - 1].nextSibling = base + static_cast<int>(i);
    }
    return nodes;
}

void VirtualTreeModel::setInitialNodes(std::vector<TreeNode> nodes)
{
    beginResetModel();
    m_nodes = std::move(nodes);
    endResetModel();
}

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
        // Root items: the row-th node with parentIndex == -1.
        int count = 0;
        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
            if (m_nodes[i].parentIndex == -1) {
                if (count == row) return createIndex(row, column, i);
                ++count;
            }
        }
        return {};
    }

    // Child items: walk the firstChild → nextSibling chain.
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
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return {};
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
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return 0;
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
    const int nodeIdx = nodeIndexOf(index);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return {};
    const auto& node = m_nodes[nodeIdx];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return node.truncated
                    ? QStringLiteral("%1  (partial — %2 children shown)")
                          .arg(node.tagName).arg(rowCount(index))
                    : node.tagName;
        case 1: return node.attrSummary;
        case 2: return node.textPreview;
        }
    }
    if (role == Qt::ToolTipRole)
        return QStringLiteral("<%1> at byte %2").arg(node.tagName).arg(node.byteOffset);
    if (role == Qt::ForegroundRole && index.column() == 0)
        return QColor(0x00, 0x55, 0xAA);
    return {};
}

QVariant VirtualTreeModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return tr("Element");
    case 1: return tr("Attributes");
    case 2: return tr("Text");
    }
    return {};
}

bool VirtualTreeModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid()) return !m_nodes.empty();
    const int nodeIdx = nodeIndexOf(parent);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return false;
    return m_nodes[nodeIdx].hasChildren;
}

bool VirtualTreeModel::canFetchMore(const QModelIndex& parent) const
{
    if (!parent.isValid()) return false;
    const int nodeIdx = nodeIndexOf(parent);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return false;
    const auto& node = m_nodes[nodeIdx];
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
    const int nodeIdx = nodeIndexOf(index);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return 0;
    return m_nodes[nodeIdx].byteOffset;
}

bool VirtualTreeModel::elementRange(const QModelIndex& index,
                                    uint64_t* start, uint64_t* end) const
{
    if (!index.isValid() || !m_doc) return false;
    const int nodeIdx = nodeIndexOf(index);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return false;

    const TreeNode& node = m_nodes[nodeIdx];
    *start = node.byteOffset;

    if (node.endOffset > node.byteOffset) { *end = node.endOffset; return true; }

    // Not known yet: walk the element's own subtree until its end tag, tracking
    // nesting so a same-named descendant cannot close it early.
    uint64_t found    = 0;
    int      relDepth = -1;
    const uint64_t limit = std::min(m_doc->length(), node.byteOffset + kMaxScanBytes);

    XmlScanner::scan(*m_doc, node.byteOffset, limit, [&](const XmlNode& n) {
        if (relDepth < 0) {
            if (n.kind == XmlNode::Kind::EmptyTag) {
                found = n.offset + n.raw.size();
                return false;
            }
            if (n.kind == XmlNode::Kind::StartTag) { relDepth = 0; return true; }
            return true;
        }
        if (n.kind == XmlNode::Kind::StartTag) ++relDepth;
        else if (n.kind == XmlNode::Kind::EndTag) {
            if (relDepth == 0) { found = n.offset + n.raw.size(); return false; }
            --relDepth;
        }
        return true;
    });

    if (found <= node.byteOffset) return false;

    // Cache it so a repeat request is free.
    const_cast<TreeNode&>(node).endOffset = found;
    *end = found;
    return true;
}

QString VirtualTreeModel::tagNameFor(const QModelIndex& index) const
{
    if (!index.isValid()) return {};
    const int nodeIdx = nodeIndexOf(index);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return {};
    return m_nodes[nodeIdx].tagName;
}

QString VirtualTreeModel::xpathFor(const QModelIndex& index) const
{
    if (!index.isValid()) return {};
    int nodeIdx = nodeIndexOf(index);
    if (nodeIdx < 0 || nodeIdx >= static_cast<int>(m_nodes.size())) return {};

    QStringList parts;
    while (nodeIdx >= 0) {
        const TreeNode& node = m_nodes[nodeIdx];

        // Position among siblings sharing this element name. Only emit the
        // predicate when the name actually repeats, keeping paths readable.
        int position = 1;
        int sameName = 0;
        const int parentIdx = node.parentIndex;
        int sib = (parentIdx < 0)
            ? firstRootNode()
            : m_nodes[parentIdx].firstChild;
        bool beforeSelf = true;
        while (sib != -1) {
            if (m_nodes[sib].tagName == node.tagName) {
                ++sameName;
                if (beforeSelf && sib != nodeIdx) ++position;
            }
            if (sib == nodeIdx) beforeSelf = false;
            sib = (parentIdx < 0) ? nextRootNode(sib) : m_nodes[sib].nextSibling;
        }

        parts.prepend(sameName > 1
            ? QStringLiteral("%1[%2]").arg(node.tagName).arg(position)
            : node.tagName);
        nodeIdx = node.parentIndex;
    }

    return QStringLiteral("/") + parts.join(QLatin1Char('/'));
}

int VirtualTreeModel::firstRootNode() const
{
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i)
        if (m_nodes[i].parentIndex == -1) return i;
    return -1;
}

int VirtualTreeModel::nextRootNode(int from) const
{
    for (int i = from + 1; i < static_cast<int>(m_nodes.size()); ++i)
        if (m_nodes[i].parentIndex == -1) return i;
    return -1;
}

QModelIndex VirtualTreeModel::indexForOffset(uint64_t offset) const
{
    // Descend through loaded nodes only: following the caret must never trigger
    // a scan, or scrolling a large document would stall.
    int best = -1;
    for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
        const auto& n = m_nodes[i];
        if (offset < n.byteOffset) continue;
        if (n.endOffset != 0 && offset >= n.endOffset) continue;
        if (best < 0 || n.depth > m_nodes[best].depth) best = i;
    }
    if (best < 0) return {};
    return createIndex(rowOfNode(best), 0, best);
}

// ── Private ──────────────────────────────────────────────────────────────────

void VirtualTreeModel::loadChildren(int nodeIndex)
{
    if (!m_doc) { m_nodes[nodeIndex].isLoaded = true; return; }

    ChildScan scan = collectChildren(*m_doc, m_nodes[nodeIndex].byteOffset,
                                     m_nodes[nodeIndex].depth, nodeIndex,
                                     kMaxChildrenPerNode, kMaxScanBytes, nullptr);
    std::vector<TreeNode>& batch = scan.children;

    m_nodes[nodeIndex].endOffset   = scan.parentEnd;
    m_nodes[nodeIndex].textPreview = scan.parentText;
    m_nodes[nodeIndex].truncated   = scan.truncated;

    if (batch.empty()) {
        // No element children after all — drop the expander.
        m_nodes[nodeIndex].isLoaded    = true;
        m_nodes[nodeIndex].hasChildren = false;
        return;
    }

    const QModelIndex parentQIdx = createIndex(rowOfNode(nodeIndex), 0, nodeIndex);
    beginInsertRows(parentQIdx, 0, static_cast<int>(batch.size()) - 1);

    const int base = static_cast<int>(m_nodes.size());
    for (int i = 0; i < static_cast<int>(batch.size()); ++i) {
        m_nodes.push_back(std::move(batch[static_cast<size_t>(i)]));
        if (i == 0) m_nodes[nodeIndex].firstChild = base;
        else        m_nodes[base + i - 1].nextSibling = base + i;
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
