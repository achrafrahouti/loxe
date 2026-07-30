#pragma once

#include <QAbstractItemModel>
#include <QString>
#include <atomic>
#include <cstdint>
#include <vector>

class PieceTable;

struct TreeNode {
    uint64_t byteOffset  = 0;   // offset of this element's '<'
    uint64_t endOffset   = 0;   // offset just past its closing '>' (0 if unknown)
    int      depth       = 0;
    QString  tagName;
    QString  attrSummary;       // [@attr="val"] shown in the Attributes column
    QString  textPreview;       // ≤ 60 chars of text content for the Text column
    bool     hasChildren = false;
    bool     isLoaded    = false;
    bool     truncated   = false; // child list was capped
    int      parentIndex = -1;
    // Children occupy the contiguous range [firstChild, firstChild + childCount)
    // of the node vector: every expansion appends its children as one batch and
    // nothing is ever removed, so a node's child range never moves. That is what
    // makes index()/rowCount() O(1) instead of a sibling-chain walk — expanding
    // 50 000 siblings through a linked chain is quadratic and took ~16 s.
    int      firstChild  = -1;
    int      childCount  = 0;
};

// QAbstractItemModel backed by a flat std::vector<TreeNode>.
//
// Each node stores only its byte offsets, depth and display strings — never
// document content. Children are discovered on demand by streaming that
// element's byte range through XmlScanner, so opening a 2 GB document costs one
// pass for the top level and nothing for the collapsed remainder.
//
// The model reads the live PieceTable, not the file on disk, so the tree stays
// consistent with unsaved edits.
class VirtualTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    // Children per expansion are capped so a document with millions of siblings
    // cannot exhaust memory; the parent is then flagged truncated.
    static constexpr int kMaxChildrenPerNode = 50000;

    // Enumerating an element's children means traversing its whole subtree, so
    // the scan is also capped by bytes. Without this, a 2 GB document whose root
    // has a handful of enormous children would take seconds to expand and blow
    // the "tree level-1 population < 3 s" budget. Whichever cap trips first
    // marks the parent truncated.
    static constexpr uint64_t kMaxScanBytes = 768ull * 1024 * 1024;

    explicit VirtualTreeModel(const PieceTable* doc, QObject* parent = nullptr);

    // Append root-level nodes (called from AsyncLoader Phase 3).
    void appendRootNodes(std::vector<TreeNode> nodes);

    // Replaces the whole node list. Used to install the pre-scanned first two
    // levels produced by AsyncLoader.
    void setInitialNodes(std::vector<TreeNode> nodes);

    // Scans the document element *and its direct children*, returning a flat
    // node list with parent/sibling links already wired: index 0 is the document
    // element, marked loaded, followed by its level-1 children.
    //
    // Runs on the loader thread on purpose. Enumerating level-1 children means
    // one pass over the whole document, which must not happen on the UI thread
    // when the tree view first expands the root.
    static std::vector<TreeNode> scanRoots(const PieceTable& doc,
                                           const std::atomic<bool>* cancelled = nullptr);

    // QAbstractItemModel interface
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int         rowCount(const QModelIndex& parent = {}) const override;
    int         columnCount(const QModelIndex& parent = {}) const override;
    QVariant    data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant    headerData(int section, Qt::Orientation orientation,
                           int role = Qt::DisplayRole) const override;
    bool        hasChildren(const QModelIndex& parent = {}) const override;
    bool        canFetchMore(const QModelIndex& parent) const override;
    void        fetchMore(const QModelIndex& parent) override;

    // Returns the byte offset for the node at index (used by ViewportRenderer sync).
    uint64_t byteOffsetFor(const QModelIndex& index) const;

    // Byte range [start, end) covering the element's full source, closing tag
    // included. The end offset is discovered lazily when it is not already
    // known, so this can cost a bounded scan of the element's subtree.
    // Returns false when the range cannot be determined.
    bool elementRange(const QModelIndex& index, uint64_t* start, uint64_t* end) const;

    // Location path to the element, with positional predicates where a name is
    // repeated among siblings, e.g. /orders/order[3]/total.
    QString xpathFor(const QModelIndex& index) const;

    QString tagNameFor(const QModelIndex& index) const;

    // Deepest already-loaded node whose byte range contains `offset`, or an
    // invalid index. Used to follow the caret in the tree without forcing loads.
    QModelIndex indexForOffset(uint64_t offset) const;

private:
    // Streams nodeIndex's byte range and populates its direct children.
    void loadChildren(int nodeIndex);

    // Returns the node index encoded in a QModelIndex internalId.
    int nodeIndexOf(const QModelIndex& idx) const;

    // Returns the row of nodeIdx among its siblings.
    int rowOfNode(int nodeIdx) const;

    // Rebuilds m_roots from m_nodes.
    void rebuildRoots();

    const PieceTable*     m_doc = nullptr;
    std::vector<TreeNode> m_nodes;
    // Indices of the top-level nodes, in row order. Kept explicitly so the root
    // rows need no scan of the whole node vector.
    std::vector<int>      m_roots;
};
